
#include <numeric>
#include <algorithm>
#include <ADC.h>
#include <vector>
#include "FS_macros.h"


ADC* adc = new ADC();

uint16_t tf_input_arr[tf_window];

int i, j;
int in[2] = { 0 , 0 };
int timeout = 0;
float drive_freqs[3];

uint16_t xDACmax = 0;
uint16_t yDACmax = 0;

bool switch_ = false;

void setup()
{
	pinMode(LED_BUILTIN, OUTPUT);

	adc->adc0->setAveraging(16);
	adc->adc0->setResolution(12);

	analogWriteResolution(12);

	Serial.begin(115200);

	post_setup();
}


void post_setup() {

	while (true) {

		digitalWriteFast(LED_BUILTIN, LOW);
		
		Serial.clear();

		while (!Serial.available());
		
		switch (Serial.read()) {

		case FIND_RANGE:
			find_range();
			break;

		case LEARN_TF:
			learn_tf();
			break;

		case STABILIZE:
			stabilize();
			break;

		default:
			taper_down();

		}

	}
}


void loop() {

}


void stabilize() {
	
	digitalWriteFast(LED_BUILTIN, HIGH);

	init_actuator();

	while (true) {

		elapsedMillis m;
		while (!Serial.available()) {
			if (m > 5000) {
				taper_down();
				return;
			}
		}

		if (Serial.read() != SYNC_FLAG) {
			Serial.clear();
		}
		else {

			Serial.readBytes((char*)in, 4);

			//analogWriteDAC1(in[0]);

			analogWriteDAC1(switch_ ? 600 : 0);
			switch_ = !switch_;

		}
	}
}

void learn_tf() {
	
	digitalWriteFast(LED_BUILTIN, HIGH);

	Serial.readBytes((char*)drive_freqs, 12);
	Serial.readBytes((char*)&yDACmax, 2);
	

	tf_input_(tf_input_arr,yDACmax,drive_freqs);

	init_actuator();

	Serial.clear();
	Serial.write((char*) CONTINUE, 1);
	
	i = 0;

	while (i < tf_window) {

		elapsedMillis m;
		while (!Serial.available()) {
			if (m > 1000) {
				taper_down();
				return;
			}
		}

		if (Serial.read() != SYNC_FLAG) {
			Serial.clear();
		}

		else {
			delayMicroseconds(25);
			//analogWriteDAC0(tf_input_arr[i]);
			analogWriteDAC1(tf_input_arr[i]);
			++i;
		}
	}
	
	taper_down();
}

void find_range() {

	digitalWriteFast(LED_BUILTIN, HIGH);

	yDACmax = 0;
	xDACmax = 0;

	int curr_x = 0;
	int curr_y = 0;

	while (true) {

		elapsedMillis m;
		while (!Serial.available()) {
			if (m > 2000) {
				taper_down();
				return;
			}
		}

		if (Serial.read() != SYNC_FLAG) {
			Serial.clear();
			Serial.write((byte)CONTINUE);
		}

		else {
			if (Serial.read() == STOP_FLAG) {
				yDACmax = curr_y - 5;
				Serial.write((byte)STOP_FLAG);
				Serial.write((const byte*)&yDACmax, 2);
				break;
			}
			else {
				analogWriteDAC1(curr_y);
				curr_y += 5;

				if (curr_y >= 4095) {
					yDACmax = 4095;
					Serial.write((byte)STOP_FLAG);
					Serial.write((const byte*)&yDACmax, 2);
					break;
				}

				Serial.write((byte)CONTINUE);
			}
		}
	}

	timeout = 0;

	while (true) {

		elapsedMillis m;
		while (!Serial.available()) {
			if (m > 2000) {
				taper_down();
				return;
			}
		}

		if (Serial.read() != SYNC_FLAG) {
			Serial.clear();
			Serial.write((byte)CONTINUE);
		}

		else {
			if (Serial.read() == STOP_FLAG) {
				xDACmax = curr_x - 5;
				Serial.write((byte)STOP_FLAG);
				Serial.write((const byte*)&xDACmax, 2);
				break;
			}
			else {
				analogWriteDAC0(curr_x);
				curr_x += 5;

				if (curr_x >= 4095) {
					xDACmax = 4095;
					Serial.write((byte)STOP_FLAG);
					Serial.write((const byte*)&xDACmax, 2);
					break;
				}

				Serial.write((byte)CONTINUE);
			}
		}
	}

	taper_down();
}

inline void taper_down() {

	int tapering_val_y = adc->adc0->analogRead(A3);
	int tapering_val_x = adc->adc0->analogRead(A9);
	
	while (tapering_val_y > 0) {
		analogWriteDAC1(--tapering_val_y);
		delayMicroseconds(100);
	}

	while (tapering_val_x > 0) {
		analogWriteDAC0(--tapering_val_x);
		delayMicroseconds(100);
	}

}

inline void init_actuator() {

	for (i = 0; i < 20; ++i) {
		analogWriteDAC0(yDACmax / 20 * i);
		analogWriteDAC1(yDACmax / 20 * i);
		delay(1);
	}

	for (i = 0; i < 20; ++i) {
		analogWriteDAC0(yDACmax - yDACmax / 20 * i);
		analogWriteDAC1(yDACmax - yDACmax / 20 * i);
		delay(1);
	}

	for (i = 0; i < 10; ++i) {
		analogWriteDAC0(round((double)yDACmax / 2 / 20 * i));
		analogWriteDAC1(round((double)yDACmax / 2 / 20 * i));
		delay(1);
	}

}

