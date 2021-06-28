#include "processing_thread.h"
//#include "nlopt.hpp"
#include <armadillo>
#include <gsl/gsl_vector.h>
#include <gsl/gsl_filter.h>
#include <gsl/gsl_math.h>
#include "SDTFT_algo.h"
#include <queue>
#include <iostream>
#include <fstream>

#define sq(x) ((x)*(x))

using namespace arma;

#define STABILIZE_DEBUG
#undef STABILIZE_DEBUG


template <typename T>
double preciserms(T& s) {

	double sumx = 0;
	double sumxx = 0;
	int fails = 0;
	for (int i = 0; i < s.size(); ++i) {

		if (!isnan(s[i])) {
			sumx += s[i];
		}
		else
			fails++;
	}

	double shift = sumx / (double)s.size();

	sumx = 0;

	for (int i = 0; i < s.size(); ++i) {

		if (!isnan(s[i])) {
			sumx += s[i] - shift;
			sumxx += (s[i] - shift) * (s[i] - shift);
		}
	}

	return sqrt((sumxx - sumx * sumx / ((double)s.size() - fails)) / (s.size() - fails));

}

using namespace std::chrono;

struct harmonics_filter {

	harmonics_filter(int freq, int filter_halfwidth) {


		fundamental = new Filter(BPF, n_taps, 1000, freq - filter_halfwidth, freq + filter_halfwidth);
		third = new Filter(BPF, n_taps, 1000, freq + 2 * freq - filter_halfwidth, freq + 2 * freq + filter_halfwidth);
		fifth = new Filter(BPF, n_taps, 1000, freq + 4 * freq - filter_halfwidth, freq + 4 * freq + filter_halfwidth);


		//passband_ripple_compensate

		std::vector<int> my_freqs = { freq, freq + 2 * freq, freq + 4 * freq };
		std::vector<double> in(1500);
		std::vector<double> out(1500);

		for (int freq : my_freqs) {

			Filter* temp = new Filter(BPF, n_taps, 1000, freq - filter_halfwidth, freq + filter_halfwidth);

			for (int i = 0; i < 1500; ++i) {
				in[i] = cos(2 * PI * freq / 1000 * i);
				out[i] = temp->do_sample(in[i]);
			}

			auto gain = std::max_element(out.begin() + n_taps + 1, out.end());

			gain_factors.push_back(*gain);

			delete temp;

		}

	}

	~harmonics_filter() {

		delete fundamental;
		delete third;
		delete fifth;

	}

	void filter_vec(std::vector<double>& to_filter) {

		double result;

		for (int i = 0; i < to_filter.size(); ++i) {

			result = fundamental->do_sample(to_filter[i]) / gain_factors[0];
			result += third->do_sample(to_filter[i]) / gain_factors[1];
			result += fifth->do_sample(to_filter[i]) / gain_factors[2];
			to_filter[i] = result;

		}

	}

	std::vector<double> gain_factors;
	Filter* fundamental;
	Filter* third;
	Filter* fifth;
};

processing_thread::processing_thread(CDeviceInfo fb_info, QObject* parent)
    : QThread(parent), fb_cam(CTlFactory::GetInstance().CreateDevice(fb_info)),
      max_DAC_range_y(-1),max_DAC_range_x(-1), monitor_cam_enabled(false)
{

	drive_freqs.fill(-1);

}

processing_thread::processing_thread(CDeviceInfo fb_info, CDeviceInfo m_info, QObject* parent)
    : QThread(parent), fb_cam(CTlFactory::GetInstance().CreateDevice(fb_info)), monitor_cam(CTlFactory::GetInstance().CreateDevice(m_info)),
      max_DAC_range_y(-1),max_DAC_range_x(-1), monitor_cam_enabled(true)
{

	drive_freqs.fill(-1);

}

processing_thread::~processing_thread()
{
}

void processing_thread::adjust_framerate() {

	//Dumb hack - this sets the acquisition frame rate to the maximum allowed with given settings
	fb_cam.AcquisitionFrameRateAbs.SetValue(100000.0);

	if (fb_cam.ResultingFrameRateAbs() > 1000)
		fb_cam.AcquisitionFrameRateAbs.SetValue(1000);
	else
		fb_cam.AcquisitionFrameRateAbs.SetValue(fb_cam.ResultingFrameRateAbs());

	if (monitor_cam_enabled) {

		monitor_cam.AcquisitionFrameRateAbs.SetValue(100000.0);

		if (monitor_cam.ResultingFrameRateAbs() > 1000)
			monitor_cam.AcquisitionFrameRateAbs.SetValue(1000);
		else
			monitor_cam.AcquisitionFrameRateAbs.SetValue(monitor_cam.ResultingFrameRateAbs());
	}
}

void processing_thread::identify_initial_vals() {

	GrabResultPtr_t ptr;
	const int height = fb_cam.Height();
	const int width = fb_cam.Width();

	fb_cam.MaxNumBuffer.SetValue(5);
	fb_cam.StartGrabbing();

	std::vector<double> centroid_x(2000);
	std::vector<double> centroid_y(2000);
	float new_centroid[2];

	qDebug() << "2 sec of noise";

	for (int i = 0; i < 2000; ++i) {

		if (fb_cam.RetrieveResult(10, ptr, Pylon::TimeoutHandling_Return)) {

			centroid(ptr, height, width, new_centroid, threshold);

			centroid_x[i] = width - new_centroid[0];
			centroid_y[i] = height - new_centroid[1];

			qDebug() << centroid_y[i];

		}
	}

	fb_cam.StopGrabbing();

}

void processing_thread::receive_large_serial_buffer(QSerialPort &teensy, std::vector<int> &buffer, int chunk_size) {
	
	const int tot_bytes = buffer.size() * sizeof(int);
	int tot_bytes_read = 0;
	int* curr_pos = buffer.data();

	while (tot_bytes_read < tot_bytes) {

		teensy.write(QByteArray(1, CONTINUE));
		teensy.waitForBytesWritten(10);

		if (teensy.waitForReadyRead(1000)) {

            int bytes_received = 0;

            while (bytes_received != chunk_size){

                bytes_received = teensy.read((char*)curr_pos, chunk_size);

                curr_pos += bytes_received / sizeof(int);

                tot_bytes_read += bytes_received;

            }
        }
    }
}

void processing_thread::test_loop_times() {

	emit write_to_log(QString("Beginning Loop Time Test..."));

	QSerialPort teensy;

    open_port(teensy);

	adjust_framerate();

	if (fb_cam.ResultingFrameRateAbs.GetValue() < 1000) {

		emit write_to_log(QString("Camera cannot acquire at 1 kHz (") + QString::number(fb_cam.ResultingFrameRateAbs()) + QString(" Hz)"));
		emit finished_analysis();
		return;
	}

	if (fb_cam.TriggerMode.GetValue() != TriggerMode_On) {
		emit write_to_log(QString("Camera must be hooked up to trigger source in order to time"));
		return;
	}

	float new_centroid[2];

	GrabResultPtr_t ptr;

	const int height = fb_cam.Height.GetValue();
	const int width = fb_cam.Width.GetValue();

	std::vector<int> computer_loop_times(5000);

	fb_cam.StartGrabbing();

	int i = 0;
	int missed = 0;

    teensy.write(QByteArray(1, TEST_LOOP_TIME));
    teensy.waitForBytesWritten(100);

	for (int i = 0; i < 5000; ++i) {
		
        if (fb_cam.RetrieveResult(100, ptr, Pylon::TimeoutHandling_Return)) {
			auto start = high_resolution_clock::now();

			centroid(ptr, height, width, new_centroid, threshold);

			teensy.write(QByteArray(1, SYNC_FLAG));
			teensy.write((const char*) &i, 4);
			teensy.waitForBytesWritten(10);
			
			computer_loop_times[i] = duration_cast<microseconds>(high_resolution_clock::now() - start).count();

		}
		else
			++missed;

	}

	fb_cam.StopGrabbing();

	std::vector<int> loop_times(5000);
	std::vector<int> shots_sent(5000);

    receive_large_serial_buffer(teensy, loop_times, 128);
    receive_large_serial_buffer(teensy, shots_sent, 128);

    teensy.close();

	time_t start = time(0);
	std::string date_time = ctime(&start);
    std::string log_file("loop_tests/loop_test_");
	log_file.append(date_time);
	log_file.erase(log_file.end() - 1);
	log_file.append(".txt");
	std::replace(log_file.begin(), log_file.end(), ':', '-');
    std::replace(log_file.begin(), log_file.end(), ' ', '_');

	std::ofstream loop_tests(log_file.c_str(), std::ofstream::out);

	loop_tests << missed << std::endl;
	for (int loop_time : loop_times)
		loop_tests << loop_time << std::endl;;
	for (int computer_loop_time : computer_loop_times)
		loop_tests << computer_loop_time << std::endl;
	for (int shot_sent : shots_sent)
		loop_tests << shot_sent << std::endl;

	loop_tests.close();

	emit finished_analysis();
}

void processing_thread::stabilize() {

	QSerialPort teensy;

    open_port(teensy);

	adjust_framerate();
	
	emit write_to_log(QString("Beginning Stabilization..."));

	if (fb_cam.ResultingFrameRateAbs.GetValue() < 1000) {

		emit write_to_log(QString("Camera cannot acquire at 1 kHz (") + QString::number(fb_cam.ResultingFrameRateAbs()) + QString(" Hz)"));
		emit finished_analysis();
		return;
	}

	for (float freq : drive_freqs) {
		if (freq > 500 || freq < 0) {
			emit write_to_log(QString("Driving frequencies have not been set or are invalid"));
			emit finished_analysis();
			return;
		}
	}

    if (max_DAC_range_y > 4095 || max_DAC_range_y < 0 || max_DAC_range_x > 4095 || max_DAC_range_x < 0) {
		emit write_to_log(QString("Actuator Range has not been set or is invalid"));
		emit finished_analysis();
		return;
	}

	int next_commands[2];

	teensy.write(QByteArray(1, STABILIZE));
	teensy.waitForBytesWritten(100);

    if (teensy.waitForReadyRead(1000)){
        teensy.clear();
        emit write_to_log("Actuator Initialized...");
    }
    else {
        teensy.clear();
        emit write_to_log(QString("Synchronization error: could not read"));
        teensy.close();
        emit finished_analysis();
        return;
    }


    //identify_initial_vals();

#ifdef STABILIZE_DEBUG

	float new_centroid[2];
	std::vector<double> noise(2000);
	for (int i = 26; i < 2026; ++i) {
		noise[i - 26] = 2 * cos(2 * PI * 110 / 1000 * i);
	}

	for (int i = 0; i < 2000; ++i) {

		new_centroid[1] = noise[i] + axes[1].target[0];

		next_commands[1] = axes[1].next_DAC(new_centroid[1]);

		qDebug() << axes[1].DAC_cmds[0];
		qDebug() << axes[1].target[0];
		qDebug() << axes[1].noise_element;
		qDebug() << new_centroid[1];

		axes[1].post_step();
	}

#else

    std::vector<SDTFT_algo> axes = {
            SDTFT_algo(fit_params[0],x_tones,x_N,max_DAC_range_x,centroid_set_points[0]),
            SDTFT_algo(fit_params[1],y_tones,y_N,max_DAC_range_y,centroid_set_points[1])};


	GrabResultPtr_t ptr;
	const int height = fb_cam.Height();
	const int width = fb_cam.Width();
	float new_centroid[2];

	fb_cam.MaxNumBuffer.SetValue(5);
    fb_cam.StartGrabbing(GrabStrategy_LatestImageOnly);
	int k = 0;
	acquiring = true;

	while (acquiring) {

		if (fb_cam.RetrieveResult(10, ptr, Pylon::TimeoutHandling_Return)) {

			centroid(ptr, height, width, new_centroid, threshold);
			ptr.Release();

            next_commands[1] = axes[1].next_DAC_cmd(height - new_centroid[1]);

			teensy.write(QByteArray(1, SYNC_FLAG));
			teensy.write((const char*)&next_commands[1], 4);
			teensy.waitForBytesWritten(10);

			//qDebug() << axes[1].DAC_cmds[0];
			//qDebug() << axes[1].target[0];
			//qDebug() << axes[1].noise_element;
			qDebug() << height - new_centroid[1];

            axes[1].prepare_next_cmd();

			++k;
			if (k == 4950)
				acquiring = false;

		}

	}

	fb_cam.StopGrabbing();

#endif

	teensy.close();

	emit finished_analysis();

}

void processing_thread::receive_cmd_line_data(QStringList cmd_str_list) {

	bool numcheck = false;

	if (cmd_str_list.length() == 3) {

		for (int i = 0; i < 3; ++i) {

			drive_freqs[i] = cmd_str_list[i].toFloat(&numcheck);

			if (numcheck == false) {
				write_to_log(QString("Could not parse driving frequencies"));
				return;
			}
		}

		write_to_log(QString("Driving frequencies have been updated to: "
			+ QString::number(drive_freqs[0]) + QString(" , ")
			+ QString::number(drive_freqs[1]) + QString(" , and ")
			+ QString::number(drive_freqs[2])) + QString(" Hz"));
	}

	if (cmd_str_list.length() == 2) {

		int DAC_range = cmd_str_list[0].toInt(&numcheck);

		if (numcheck == false || DAC_range > 4095 || DAC_range < 0) {
			write_to_log(QString("Could not parse DAC range, enter a number between 0 and 4095"));
			return;
		}

        max_DAC_range_x = DAC_range;

		DAC_range = cmd_str_list[1].toInt(&numcheck);

		if (numcheck == false || DAC_range > 4095 || DAC_range < 0) {
			write_to_log(QString("Could not parse DAC range, enter a number between 0 and 4095"));
			return;
		}

        max_DAC_range_y = DAC_range;

		write_to_log(QString("DAC range in x: ")
            + QString::number(max_DAC_range_x));
		write_to_log(QString("DAC range in y: ")
            + QString::number(max_DAC_range_y));

	}
}

void processing_thread::stream() {

	adjust_framerate();

	const int update_period = 20; // ms

	acquiring = true;

	if (monitor_cam_enabled) {

		fb_cam.StartGrabbing();
		monitor_cam.StartGrabbing();

		GrabResultPtr_t fb_ptr;
		GrabResultPtr_t m_ptr;

		while (acquiring) {

			msleep(update_period);

			if (fb_cam.RetrieveResult(5000, fb_ptr, Pylon::TimeoutHandling_Return))
				emit send_feedback_ptr(fb_ptr);

			if (monitor_cam.RetrieveResult(5000, m_ptr, Pylon::TimeoutHandling_Return))
				emit send_monitor_ptr(m_ptr);
		
		}

		fb_cam.StopGrabbing();
		monitor_cam.StopGrabbing();
	}

	else {

		fb_cam.StartGrabbing();

		while (acquiring) {

			msleep(update_period);

			GrabResultPtr_t fb_ptr;

			if (fb_cam.RetrieveResult(100, fb_ptr, Pylon::TimeoutHandling_Return)) {

				emit send_feedback_ptr(fb_ptr);

                std::array<float, 2> p = centroid<float>(fb_ptr, threshold);

			}
		}

		fb_cam.StopGrabbing();
	}

}

void processing_thread::analyze_spectrum() {

	adjust_framerate();

	emit write_to_log(QString("Beginning Noise Profiling..."));

	if (fb_cam.ResultingFrameRateAbs.GetValue() < 1000) {

		emit write_to_log(QString("Camera cannot acquire at 1 kHz (") + QString::number(fb_cam.ResultingFrameRateAbs()) + QString(" Hz)"));
		emit finished_analysis();
		return;
	}


    centroidx_d.resize(_window);
    centroidy_d.resize(_window);


	std::vector<GrabResultPtr_t> ptrs(_window);

	int missed = 0;
	int i = 0;


	fb_cam.MaxNumBuffer.SetValue(_window);
	//fb_cam.GevSCPSPacketSize.SetValue(((fb_cam.Height.GetValue() * fb_cam.Width.GetValue() + 14) / 4) * 4);
	fb_cam.StartGrabbing();

	while (i < _window) {
		if (fb_cam.RetrieveResult(30, ptrs[i], Pylon::TimeoutHandling_Return)) {
			++i;
		}
		else {
			++missed;
		}

		if (i % (_window / 100) == 0) {
			emit updateprogress(i);
		}
	}

	fb_cam.StopGrabbing();

	emit updateprogress(_window);

	emit write_to_log(QString::number(i) + QString(" Shots recorded"));

	emit write_to_log(QString::number(missed) + QString(" Shots missed"));

    std::function<std::array<double, 2>(GrabResultPtr_t)> calc;

	calc = [thresh = threshold](GrabResultPtr_t pt) {
        return centroid<double>(pt, thresh);};

    QFuture<std::array<double, 2>> centroids = QtConcurrent::mapped(ptrs.begin(), ptrs.end(), calc);

	centroids.waitForFinished();

	float sumx = 0;
	float sumy = 0;
	int nans = 0;

	for (int i = 0; i < _window; ++i) {
		if (std::isnan(centroids.resultAt(i)[0]))
			++nans;

        centroidx_d[i] = centroids.resultAt(i)[0];
        sumx += centroidx_d[i];

		if (std::isnan(centroids.resultAt(i)[1]))
			++nans;

        centroidy_d[i] = centroids.resultAt(i)[1];
        sumy += centroidy_d[i];
	}

	float meanx = sumx / _window;
	float meany = sumy / _window;


	for (int i = 0; i < _window; ++i) {
		centroidx_f[i] -= meanx;
		centroidy_f[i] -= meany;
	}

	write_to_log(QString::number(nans) + QString(" NaN centroids detected"));


    fftw_plan plan_x = fftw_plan_r2r_1d(_window, centroidx_d.data(), fft_out_x, FFTW_R2HC, FFTW_ESTIMATE);
    fftw_plan plan_y = fftw_plan_r2r_1d(_window, centroidy_d.data(), fft_out_y, FFTW_R2HC, FFTW_ESTIMATE);

    fftw_execute(plan_x);
    fftw_execute(plan_y);

    fft_x.resize(_window / 2);
    fft_y.resize(_window / 2);


    fft_x[0] = sqrt(fft_out_x[0] * fft_out_x[0]);
    fft_y[0] = sqrt(fft_out_y[0] * fft_out_y[0]);


	for (int i = 1; i < (_window + 1) / 2 - 1; ++i) {

        fft_x[i] = sqrt(fft_out_x[i] * fft_out_x[i] +
            fft_out_x[_window - i] * fft_out_x[_window - i]);
	}

	for (int i = 1; i < (_window + 1) / 2 - 1; ++i) {
        fft_y[i] = sqrt(fft_out_y[i] * fft_out_y[i] +
            fft_out_y[_window - i] * fft_out_y[_window - i]);
	}

    double maxX = *(std::max_element(fft_x.begin(), fft_x.end()));
    float maxY = *(std::max_element(fft_y.begin(), fft_y.end()));

	for (int i = 0; i < _window / 2; ++i) {
        fft_x[i] = fft_x[i] / maxX;
        fft_y[i] = fft_y[i] / maxY;

	}

	find_driving_freqs();

    emit update_fft_plot(preciserms(centroidx_d), preciserms(centroidy_d),
        *std::max_element(centroidx_d.constBegin(), centroidx_d.constEnd()) - *std::min_element(centroidx_d.constBegin(), centroidx_d.constEnd()),
        *std::max_element(centroidy_d.constBegin(), centroidy_d.constEnd()) - *std::min_element(centroidy_d.constBegin(), centroidy_d.constEnd()));

    fftw_destroy_plan(plan_x);
    fftw_destroy_plan(plan_y);


	emit finished_analysis();

}

void processing_thread::find_driving_freqs() {

	gsl_vector* convx = gsl_vector_alloc(_window / 2 / 5);
	gsl_vector* convy = gsl_vector_alloc(_window / 2 / 5);

	gsl_filter_median_workspace* filt = gsl_filter_median_alloc(10);

	for (int i = 0; i < _window / 2 / 5; ++i) {
        gsl_vector_set(convx, i, fft_x[i] + 0.2 * fft_x[i + 2 * i] + 0.1 * fft_x[i + 4 * i]);
        gsl_vector_set(convy, i, fft_y[i] + 0.2 * fft_y[i + 2 * i] + 0.1 * fft_y[i + 4 * i]);
	}

    gsl_vector* filteredy = gsl_vector_alloc(_window / 2 / 5);
    gsl_vector* filteredx = gsl_vector_alloc(_window / 2 / 5);

	gsl_filter_median(GSL_FILTER_END_TRUNCATE, convy, filteredy, filt);
	gsl_filter_median(GSL_FILTER_END_TRUNCATE, convx, filteredx, filt);

	// First third
	gsl_vector_view y1 = gsl_vector_subvector(filteredy, 5 * _window / 2 / 500, _window / 2 / 5 / 3 - 5 * _window / 2 / 500);

	// Second third
	gsl_vector_view y2 = gsl_vector_subvector(filteredy, _window / 2 / 5 / 3, _window / 2 / 5 / 3);

	// Last third
	gsl_vector_view y3 = gsl_vector_subvector(filteredy, 2 * _window / 2 / 5 / 3, _window / 2 / 5 / 3 - 10 * _window / 2 / 500);

	drive_freqs[0] = (gsl_vector_min_index(&y1.vector) + 5 * _window / 2 / 500) / 5;
	drive_freqs[1] = (gsl_vector_min_index(&y2.vector) + _window / 2 / 5 / 3) / 5;
	drive_freqs[2] = (gsl_vector_min_index(&y3.vector) + 2 * _window / 2 / 5 / 3) / 5;

	emit write_to_log(" Drive frequencies : " + QString::number(drive_freqs[0]) + " Hz, "
		+ QString::number(drive_freqs[1]) + " Hz, "
		+ QString::number(drive_freqs[2]) + " Hz");


}

void processing_thread::open_port(QSerialPort& teensy) {
	
	QList<QSerialPortInfo> ports = QSerialPortInfo::availablePorts();

	for (QSerialPortInfo port : ports) {

		port.vendorIdentifier();

		if (port.vendorIdentifier() == 5824) {
			teensy.setPortName(port.portName());

		}
	}

	if (!teensy.open(QIODevice::ReadWrite)) {
		emit write_to_log(QString("USB port not able to open."));
		return;
	};

	bool j = true;

	j = teensy.setBaudRate(QSerialPort::Baud115200);
	j = teensy.setDataBits(QSerialPort::Data8);
	j = teensy.setParity(QSerialPort::NoParity);
	j = teensy.setStopBits(QSerialPort::OneStop);
	j = teensy.setFlowControl(QSerialPort::NoFlowControl);

	if (!j) {
		emit write_to_log(QString("Error configuring USB connection."));
		return;
	}
	else {
		emit write_to_log(QString("Port opened and configured"));
	}

}

void processing_thread::find_actuator_range() {

	adjust_framerate();

	emit write_to_log(QString("Finding Actuator Range..."));

	QSerialPort teensy;

	open_port(teensy);

	GrabResultPtr_t ptr;

	teensy.write(QByteArray(1, FIND_RANGE));
	teensy.waitForBytesWritten(50);
	fb_cam.StartGrabbing(Pylon::GrabStrategy_UpcomingImage);

#pragma region Y_RANGE

	while (true) {

		if (fb_cam.RetrieveResult(30, ptr, Pylon::TimeoutHandling_Return)) {

			std::array<double, 6> out = allparams(ptr, threshold);

			qDebug() << ptr->GetHeight() - out[1];

			if (!isnan(out[3]) && out[3] < fb_cam.Height() && (out[1] - out[3] / 2) < 0) {
				teensy.write(QByteArray(1, SYNC_FLAG));
				teensy.write(QByteArray(1, STOP_FLAG));
				if (!teensy.waitForBytesWritten(1000)) {
					emit write_to_log(QString("Synchronization error: could not write"));
					break;
				}
			}

			else {
				teensy.write(QByteArray(1, SYNC_FLAG));
				teensy.write(QByteArray(1, CONTINUE));
				if (!teensy.waitForBytesWritten(1000)) {
					emit write_to_log(QString("Synchronization error: could not write"));
					break;
				}
			}

			if (teensy.waitForReadyRead(1000)) {
				if (*teensy.read(1) == STOP_FLAG) {
                    max_DAC_range_y = *((uint16_t*)teensy.read(2).data());
                    emit write_to_log(QString("Success: the y axis DAC uses " + QString::number(max_DAC_range_y + 1) +
						" out of 4096 available units of range"));
					break;
				}
			}

			else {
				emit write_to_log(QString("Synchronization error: could not read"));
				break;
			}

			emit send_imgptr_blocking(ptr);

		}
	}
#pragma endregion

#pragma region X_RANGE

	while (true) {

		if (fb_cam.RetrieveResult(30, ptr, Pylon::TimeoutHandling_Return)) {

			std::array<double, 6> out = allparams(ptr, threshold);

			qDebug() << ptr->GetWidth() - out[0];

			if (!isnan(out[2]) && out[2] < fb_cam.Width() && (out[0] - out[2] / 2) < 0) {
				teensy.write(QByteArray(1, SYNC_FLAG));
				teensy.write(QByteArray(1, STOP_FLAG));
				if (!teensy.waitForBytesWritten(1000)) {
					emit write_to_log(QString("Synchronization error: could not write"));
					break;
				}
			}

			else {
				teensy.write(QByteArray(1, SYNC_FLAG));
				teensy.write(QByteArray(1, CONTINUE));
				if (!teensy.waitForBytesWritten(1000)) {
					emit write_to_log(QString("Synchronization error: could not write"));
					break;
				}
			}

			if (teensy.waitForReadyRead(1000)) {
				if (*teensy.read(1) == STOP_FLAG) {
                    max_DAC_range_x = *((uint16_t*)teensy.read(2).data());
                    emit write_to_log(QString("Success: the x axis DAC uses " + QString::number(max_DAC_range_x + 1) +
						" out of 4096 available units of range"));
					break;
				}
			}

			else {
				emit write_to_log(QString("Synchronization error: could not read"));
				break;
			}

			emit send_imgptr_blocking(ptr);

		}


	}

#pragma endregion

	fb_cam.StopGrabbing();

	teensy.close();

	emit finished_analysis();
}

void processing_thread::learn_system_response() {

    emit write_to_log(QString("Finding Pixel/DAC output system response..."));

	adjust_framerate();

	if (fb_cam.ResultingFrameRateAbs.GetValue() < 1000) {
		emit write_to_log(QString("Camera cannot acquire at 1 kHz") + QString::number(fb_cam.ResultingFrameRateAbs()) + QString(" Hz)"));
	}
	else {

        centroidx_d.resize(sys_response_window);
        centroidy_d.resize(sys_response_window);

		QSerialPort teensy;

        open_port(teensy);

        system_response_input.resize(sys_response_window);

        generate_system_response_input(system_response_input.data(), max_DAC_range_y, drive_freqs.data());

		GrabResultPtr_t ptr;

        teensy.write(QByteArray(1, LEARN_SYS_RESPONSE));

		teensy.write((const char*)drive_freqs.data(), 12);
        teensy.write((const char*)&max_DAC_range_y, 2);

		if (!teensy.waitForBytesWritten(100)) {
			emit write_to_log(QString("Synchronization error: could not write"));
			teensy.close();
			emit finished_analysis();
			return;
		}

		if (teensy.waitForReadyRead(1000)) {
			teensy.clear();
		}
		else {
			teensy.clear();
			emit write_to_log(QString("Synchronization error: could not read"));
			teensy.close();
			emit finished_analysis();
			return;
		}

		int missed = 0;
		int i = 0;
		const int height = fb_cam.Height();
		const int width = fb_cam.Width();
		float new_centroid[2];

        fb_cam.MaxNumBuffer.SetValue(sys_response_window);
		fb_cam.StartGrabbing();

        while (i < sys_response_window) {

			if (fb_cam.RetrieveResult(2, ptr, Pylon::TimeoutHandling_Return)) {

				centroid(ptr, height, width, new_centroid, threshold);

				centroidx_d[i] = width - new_centroid[0];
				centroidy_d[i] = height - new_centroid[1];

				teensy.write(QByteArray(1, SYNC_FLAG));
				if (!teensy.waitForBytesWritten(100)) {
					emit write_to_log(QString("Synchronization error: could not write"));
					break;
				}
				++i;
			}
			else {
				++missed;
			}

		}

		fb_cam.StopGrabbing();
		teensy.close();

        // ?????????

		centroidx_d.pop_front();
		centroidy_d.pop_front();
        system_response_input.pop_back();

        emit updateprogress(sys_response_window);

		std::vector<QVector<double>*> xy = { &centroidx_d,&centroidy_d };
		std::vector<mat> sols;

		QVector<QVector<double>> to_plot;

		fit_params.clear();
		std::vector<double> model_RMSE;

		for (QVector<double>* centroid : xy) {

			vec dDAC_full;
			vec ddDAC_full;
			vec dcentroid_full;
			vec DAC_full;
			vec centroid_full;

			double sum = std::accumulate(centroid->begin(), centroid->end(), 0.0);
			double centroid_mean = sum / centroid->size();

			//these mean values are used as the set point when stabilizing
            centroid_set_points.push_back(centroid_mean);

			for (int freq_idx = 0; freq_idx < 3; ++freq_idx) {

                // EXTRACT SECTION (A SINGLE FREQUNCY COMPONENT)
                std::vector<double> input_section((double*)system_response_input.data() + freq_idx * section_width, (double*)system_response_input.data() + (freq_idx + 1) * section_width);
				std::vector<double> centroid_section((double*)centroid->data() + freq_idx * section_width, (double*)centroid->data() + (freq_idx + 1) * section_width);

				//-----------------------------------------------------

				// FILTER
				harmonics_filter filter(drive_freqs[freq_idx], 2);
				filter.filter_vec(centroid_section);

				//-----------------------------------------------------

				// ERASE FILTER LAG INTRODUCED BY PADDING ZEROS (n_taps total)
                input_section.erase(input_section.begin(), input_section.begin() + n_taps / 2);
                input_section.erase(input_section.end() - n_taps / 2, input_section.end());
				centroid_section.erase(centroid_section.begin(), centroid_section.begin() + 2 * (n_taps / 2));

				sum = std::accumulate(centroid_section.begin(), centroid_section.end(), 0.0);
				double filtered_mean = sum / centroid_section.size();


				// FIX MEAN OFFSET INTRODUCED BY FILTER
				for (double& d : centroid_section)
					d += centroid_mean - filtered_mean;


                DAC_full = join_cols(DAC_full, vec(input_section).subvec(2, input_section.size() - 1));
				centroid_full = join_cols(centroid_full, vec(centroid_section).subvec(2, centroid_section.size() - 1));

                dDAC_full = join_cols(dDAC_full, diff(vec(input_section).subvec(1, input_section.size() - 1)));
				//dcentroid_full = join_cols(dcentroid_full, diff(vec(tf_input_section).subvec(1,centroid_in.size()-1)));

                ddDAC_full = join_cols(ddDAC_full, diff(vec(input_section), 2));

			}

            mat A_B = solve(join_rows(DAC_full, ones<vec>(DAC_full.n_rows)), centroid_full);

            vec errors = centroid_full - (A_B(0) * DAC_full + A_B(1));

            mat C_D = solve(join_rows(dDAC_full, ddDAC_full), errors);

            std::array<double, 4> fit = { A_B(0), A_B(1), C_D(0), C_D(1) };

			fit_params.push_back(fit);

            model_RMSE.push_back(sqrt(arma::sum(square(errors - (C_D(0) * dDAC_full + C_D(1) * (ddDAC_full)))) / ddDAC_full.n_rows));

            vec simulated = centroid_full + errors - (C_D(0) * dDAC_full + C_D(1) * (ddDAC_full));

            //QCustomPlot wants QVectors
			to_plot.push_back(QVector<double>::fromStdVector(arma::conv_to<std::vector<double>>::from(DAC_full)));
			to_plot.push_back(QVector<double>::fromStdVector(arma::conv_to<std::vector<double>>::from(centroid_full)));
			to_plot.push_back(QVector<double>::fromStdVector(arma::conv_to<std::vector<double>>::from(simulated)));

		}


		emit write_to_log(" centroid_y = " + QString::number(fit_params[1][0], 'e', 2) + "y + " +
			QString::number(fit_params[1][1], 'e', 2) + " - (" +
			QString::number(fit_params[1][2], 'e', 2) + "dy + " +
			QString::number(fit_params[1][3], 'e', 2) + " dcentroid_y) ");

		emit write_to_log(" centroid_x = " + QString::number(fit_params[1][0], 'e', 2) + "x + " +
			QString::number(fit_params[1][1], 'e', 2) + " - (" +
			QString::number(fit_params[1][2], 'e', 2) + "dx + " +
			QString::number(fit_params[1][3], 'e', 2) + " dcentroid_x) ");


		emit write_to_log(" x model RMSE: " + QString::number(model_RMSE[0], 'g', 2) + " px");
		emit write_to_log(" y model RMSE: " + QString::number(model_RMSE[1], 'g', 2) + " px");

        emit update_sys_response_plot(to_plot);

		emit finished_analysis();
	}

}

void processing_thread::correlate_cameras() {

	emit write_to_log(QString("Finding Linear Feedback/Monitor Camera Correlation..."));

	adjust_framerate();

	if (fb_cam.ResultingFrameRateAbs.GetValue() < 1000) {
		emit write_to_log(QString("feedback camera cannot acquire at 1 kHz (") + QString::number(fb_cam.ResultingFrameRateAbs()) + QString(" Hz)"));
		return;
	}
	else if (monitor_cam.ResultingFrameRateAbs.GetValue() < 1000) {
		emit write_to_log(QString("monitor camera cannot acquire at 1 kHz (") + QString::number(monitor_cam.ResultingFrameRateAbs()) + QString(" Hz)"));
		return;
	}
	else if (fb_cam.TriggerMode.GetValue() != TriggerModeEnums::TriggerMode_On
		|| monitor_cam.TriggerMode.GetValue() != TriggerModeEnums::TriggerMode_On) {
		emit write_to_log(QString("Both cameras must be triggered for synchronous capture"));
		return;
	}
	else {

		//qDebug() << fb_cam.ReadoutTimeAbs.GetValue();

		//fb_cam.GevIEEE1588.SetValue(true);
		//monitor_cam.GevIEEE1588.SetValue(true);
		//qDebug() << fb_cam.GevIEEE1588Status.ToString();
		//qDebug() << monitor_cam.GevIEEE1588Status.ToString();
		//
		//for (int k = 0; k < 30; ++k) {
		//	msleep(500);
		//	qDebug() << "monitor cam: " << monitor_cam.GevIEEE1588Status.ToString();
		//	qDebug() << "feedback cam: " << fb_cam.GevIEEE1588Status.ToString();
		//}


		IPylonDevice* fb_device = fb_cam.DetachDevice();
		IPylonDevice* monitor_device = monitor_cam.DetachDevice();

		CInstantCameraArray arr(2);

		arr[0].Attach(fb_device);
		arr[1].Attach(monitor_device);

		arr.StartGrabbing();
		
		CGrabResultPtr img_ptr;
		
		for (int i = 0; i < 30; ++i) {
			arr.RetrieveResult(300, img_ptr);
			qDebug() << img_ptr->GetCameraContext();
		}

		arr.StopGrabbing();

		fb_cam.Attach(arr[0].DetachDevice());
		monitor_cam.Attach(arr[1].DetachDevice());

	}

}

void processing_thread::run() {

    switch (run_plan) {
	case STABILIZE:
		stabilize();
		break;
	case STREAM:
		stream();
		break;
	case SPECTRUM:
		analyze_spectrum();
		break;
	case FIND_RANGE:
		find_actuator_range();
		break;
    case LEARN_SYS_RESPONSE:
        learn_system_response();
		break;
	case CORRELATE:
		correlate_cameras();
		break;
	case TEST_LOOP_TIME:
		test_loop_times();
		break;
	default:
		break;
	}
}
