/*
	RS_Mainboard.ino - Code to control the Mainboard for the Resistor Sortation Project
	Created by Shawn Westcott (www.8tinybits.com), Feb 2017.

	Schematics, Gerbers, and related source code can be found on this project's Github
	https://github.com/slwstctt/ResistorSortSystem

	This code is currently unlicensed and is only available for educational purposes.
	If you'd like to use this code for a non-educational purpose, please contact Shawn
	Westcott (shawn.westcott@8tinybits.com).

	NOTE: This sketch requires a SERIAL1_RX_BUFFER_SIZE and SERIAL1_TX_BUFFER_SIZE of 100
	Make this change in the teensy3 core before trying to use this sketch.

*/

#include <Arduino.h>
#include "ProgmemData.h"

#include <PWMServo.h>
#include <Wire.h>
#include <ShiftRegister74HC595.h>		// See: http://shiftregister.simsso.de/

#include "StepFeed.h"
#include "SortWheel.h"

#include <ADC.h>

// Setting up ADC object
ADC *adc = new ADC();

// Declaring Servos. ContactArm presses contacts onto resistors for measurement, SwingArm releases and retains resistors.
PWMServo ContactArm;
PWMServo SwingArm;

// Init the SortWheel and StepFeed objects
SortWheel Wheel(cupCount, SortController);
StepFeed Feed(FeedController);

// Number of shift registers in circuit
const int srCount = 2;

// Declaring the Shift Register stack.
ShiftRegister74HC595 ShiftReg(srCount, DAT0, SCLK0, LCLK0);

// Shift register states
uint8_t srState[10][srCount] = {
	{B00000000, B00000000},	// Empty
	{B10000000, B00000000},	// 10M Range		NOT USED
	{B01000000, B00000000},	// 1M Range		NOT USED
	{B00000100, B00000000},	// 100k Range
	{B00001000, B00000000},	// 10k Range
	{B00010000, B00000000},	// 1k Range
	{B00100000, B00000000},	// 100R Range
	{B00000010, B01000000},	// 100mA Source	NEEDS CALIBRATION
	{B00000001, B01000000},	// 29.37mA Source	NEEDS CALIBRATION
	{B00000000, B11000000} 	// 18.18mA Source	NEEDS CALIBRATION
};

// These variables keep track of states of slave processors.
volatile bool feedInProcess = false;
volatile bool sortMotionInProcess = false;

// Maximum Analog Value, calculated at setup.
int maxAnalog = 0;

// State Machine Variable
int cState = 0;

// Global rep of measurement data
double measurement = 0.0;

// Bool set when feeding out the remainder of the feed stack
bool feedToEnd = false;
bool isQCR = false;

void setup() {
	// Init Servos
	ContactArm.attach(SrvoA);
	SwingArm.attach(SrvoB);

	// Home the Servos
	SwingArm.write(swingHome);
	ContactArm.write(contactHome);

	// Setting up various pins
	pinMode(OE0, OUTPUT);
	pinMode(RST0, OUTPUT);
	pinMode(AttTrig0, INPUT_PULLUP);	// Trigger from Sort Controller indicating last command completed successfully
	pinMode(AttTrig1, OUTPUT);
	pinMode(AttTrig2, OUTPUT);
	pinMode(AttTrig3, INPUT_PULLUP);	// Trigger from Feed Controller indicating last command completed successfully
	pinMode(AttTrig4, OUTPUT);
	pinMode(ledPin, OUTPUT);

	// AttTrig3 is an interrupt from the slave processor that lets us know the last feed command is complete.
	attachInterrupt(AttTrig3, isrFeedClear, RISING);

	// AttTrig0 is an interrupt from the slave processor that lets us know the last sort move command is complete.
	attachInterrupt(AttTrig0, isrWheelClear, RISING);

	// Reset and enable output on the shift register
	clearRegisters();
	digitalWrite(OE0, LOW);		// Output Enable is Active-low
	digitalWrite(ledPin, HIGH);

	// I2C Wire init
	Wire.begin();

	// Wait 100ms just to make sure the Feed Controller is finished with setup before sending it data
	delay(100);

	// Send setup value to StepFeeder controller
	Wire.beginTransmission(FeedController);
	
	// Number of steps per feed action = 115 degrees, 1.8 deg/step = ~64 steps per action.
	Wire.write(64);
	Wire.endTransmission();
	
	// Set up ADC
	adc->setReference(ADC_REF_EXT);
	adc->setResolution(bitPrecision);
	adc->setAveraging(32);
	maxAnalog = adc->getMaxValue();
	adc->setSamplingSpeed(ADC_VERY_LOW_SPEED);
	adc->setConversionSpeed(ADC_VERY_LOW_SPEED);

	// Begin Serial comms with RPi
	Serial.begin(9600);

	// Wait for the Ready from the RPi.
	do {
		;
	} while (!cmdReady());

	String incCmd = Serial.readString();
	Command handshake = parseCmd(incCmd);

	if (handshake.cmd == "RDY") {
		digitalWrite(ledPin, LOW);
	} else {
		sendError("Inv Handshake on Startup. Halting.");
		while (true) { ; }	// Something went very wrong. Stop here.
	}
}

void loop() {

	Command thisCommand;

	// At the start of every loop, check if a command is waiting.
	if (cmdReady()) {
		String incCmd = Serial.readString();
		thisCommand = parseCmd(incCmd);

		if (cState == 0) {
			// Some command handling...
			if (thisCommand.cmd == "MAJ") {
				// "Major Divisions" is a common preset.
				int precisionPercent = thisCommand.args[0].toInt();
				double precision = (precisionPercent / 100.0);

				// Each cup gets a range of 10 up to 82, in powers of 10 (82 is the high side standard resistance value at the highest precision)
				for (int i = 0; i < 6; i++) {
					double lowSide = pow(10.0, i);
					double highSide;

					// Fetch high side value based on precision
					if (precisionPercent == 1) {
						highSide = (lowSide * stdResistors1[E96Count - 1]) / 100.0;
					} else if (precisionPercent == 2 || precisionPercent == 5) {
						highSide = (lowSide * stdResistors2_5[E24Count - 1]) / 100.0;
					} else {
						highSide = (lowSide * stdResistors10[E12Count - 1]) / 100.0;
					}

					Wheel.cups[i].setCupRange(getMin(lowSide, precision), getMax(highSide, precision));
					Wheel.cups[i].setRejectState(false);
				}

				// Upper end cups are rejects for this range.
				Wheel.cups[6].setRejectState(true);
				Wheel.cups[7].setRejectState(true);
				Wheel.cups[8].setRejectState(true);
				Wheel.cups[9].setRejectState(true);

				sendAck();
			}

			if (thisCommand.cmd == "DIV") {
				// "Simple Division" -- we get a number of groups to divide into, a precision, and a range of typical values.

				// TODO: This stuff.

				sendAck();
			}

			if (thisCommand.cmd == "SGL" || thisCommand.cmd == "QCR") {
				// "Single Resistance" -- We search a collection of resistors for a specific value within a given precision.
				// "Quality Check" -- Effectively the same as searching for a single resistance, except we set a flag to send measurement data.

				int precision = thisCommand.args[0].toInt();
				double nominal = thisCommand.args[1].toFloat();

				// To do this, we simply set one cup to take this value, and the rest become rejects.
				Wheel.cups[0].setCupRange(nominal, precision);
				Wheel.cups[0].setRejectState(false);

				for (int i = 1; i < cupCount; i++) {
					Wheel.cups[i].setRejectState(true);
				}

				if (thisCommand.cmd == "QCR") {
					isQCR = true;
				}

				sendAck();

			}

			if (thisCommand.cmd == "SSR") {
				// "Sortable Range" -- we get a precision and 9 typical values. The 10th is reject.
				int precision = thisCommand.args[0].toInt();

				// Loop through the list of arguments setting up cups
				for (int i = 1; i < thisCommand.numArgs; i++) {
					Wheel.cups[i].setCupRange(thisCommand.args[i].toFloat(), precision);
					Wheel.cups[i].setRejectState(false);
				}

				Wheel.cups[9].setRejectState(true);

				sendAck();

			}

			if (thisCommand.cmd == "OHM") {
				// "Ohmmeter mode" -- We set all cups to accept all resistors. Since we always send measurement data back, there's no need to do much else.

				for (int i = 0; i < cupCount; i++) {
					Wheel.cups[i].setCupRange(0.0, 1000000000.0);
					Wheel.cups[i].setRejectState(false);
				}

				sendAck();

			}

			if (thisCommand.cmd == "CUP") {
				// "Cup set" command sets a cup to a specific value.
				int cupNum = thisCommand.args[0].toInt();
				double minVal = thisCommand.args[1].toFloat();
				double maxVal = thisCommand.args[2].toFloat();

				bool isReject;

				if (thisCommand.args[3].toInt() == 0) {
					isReject = false;
				} else {
					isReject = true;
				}

				// Convert to 0-index
				cupNum--;

				Wheel.cups[cupNum].setCupRange(minVal, maxVal);
				Wheel.cups[cupNum].setRejectState(isReject);

				sendAck();

			}
		}
	}

	// The loop is a state machine, the action the system takes depends on what state it is in.
	if (cState == 0) {
		// Waiting for SRT.	
		if (thisCommand.cmd == "SRT") {
			cState = 1;
			sendReady();
		}
	} else if (cState == 1) {
		// Sorting Mode (Waiting on NXT).
		// NXT is the command that indicates the user has pressed the button saying they loaded a resistor.
		if (thisCommand.cmd == "NXT") {

			if (feedInProcess) {
				sendError("Feed In Process");
				return;
			} else if (!Feed.loadPlatformEmpty()) {
				sendError("Load Platform Not Empty");
				return;
			} else {
				Feed.load();
				cState = 2;		// Feed Process
				sendAck();
				return;
			}
		}

		// End is the user requesting that we cycle to completion.
		if (thisCommand.cmd == "END") {
			feedToEnd = true;
			cState = 2;			// Feed Process
			sendAck();
			return;
		}

	} else if (cState == 2) {
		// Resistor Feeding
		if (Feed.loadPlatformEmpty()) {
			// If the feed platform is empty, but we're in a motion, wait.
			if (feedInProcess) {
				return;
			}

			// If we're not feeding to the end after waiting, we're clear for a new command.
			if (!feedToEnd) {
				cState = 1;		// Ready for next command
				sendReady();
				return;
			}
		}

		// Because of returns, we only get to this point if the load platform is full.
		// Structuring in this way allows a states where we are feeding to the end to fall through to this point.

		if (!Feed.measurePlatformEmpty()) {
			// If the measurement platform is full, we have to handle that first.
			cState = 3;				// Measure Resistor
		} else {
			if (Feed.feedEmpty()) {
				// If the feed is empty, we must be ready.
				if (feedToEnd) {
					cState = 0;			// Finished.
					isQCR = false;			// In case we were in QCR, reset it.
					feedToEnd = false;
					sendDone();
				} else {
					cState = 1;			// Ready for next command
					sendReady();
				}
			} else {
				// Otherwise, cycle the feed and mark the motion in process.
				Feed.cycleFeed(1);
				feedInProcess = true;
			}
		}

		return;

	} else if (cState == 3) {
		// Measure Resistor
		if (!Feed.measurePlatformEmpty()) {
			// If the measurement platform isn't empty, measure the resistor
			measurement = measureResistor();

			// Get the target cup and begin the sort motion
			int targetSortPos = getTargetCup(measurement);

			// Report the measurement
			Command measurementData;
			measurementData.cmd = "MES";
			measurementData.numArgs = 2;
			measurementData.args[0] = String(targetSortPos);
			measurementData.args[1] = String(measurement, 4);
			sendCommand(measurementData);

			// Move the sort wheel.
			Wheel.moveTo(targetSortPos);
			sortMotionInProcess = true;
			cState = 4;				// Dispense Resistor
		} else {
			// If it's empty, we either need to go back to feeding or attempt dispense again.
			if (sortMotionInProcess) {
				cState = 4;			// Dispense Resistor
			} else {
				cState = 2;			// Feed Process
			}
		}

		return;

	} else if (cState == 4) {
		// Dispense Resistor
		if (!sortMotionInProcess) {
			// A dispense state occurs after a sort motion has begun. Wait for the sort motion to complete and dispense. EZPZ.
			SwingArm.write(swingOpen);
			delay(swingTime);			// actual delay here, since we shouldn't move or process anything else until we're sure this is clear.
			Feed.dispense();
			SwingArm.write(swingHome);
			delay(swingTime);
			cState = 2;				// Feed Process
		}
	}
}

void isrFeedClear() {
	feedInProcess = false;
	digitalWrite(ledPin, LOW);
}

void isrWheelClear() {
	sortMotionInProcess = false;
	digitalWrite(ledPin, LOW);
}

Command parseCmd(String incCmd) {
	// parses a command string into the Command struct. Also handles certain vital commands, such as Halt.
	
	Command output;
	
	// Start by deleting the verification bit
	incCmd.remove(0, 1);

	// Fetch the first 3 characters and delete them, with the semicolon.
	output.cmd = incCmd.substring(0, 3);
	incCmd.remove(0, 4);

	int argIndex = 0;
	int commaIndex = incCmd.indexOf(',');

	// Fetch all args until there are no delimiters left
	while (commaIndex != -1) {
		output.args[argIndex] = incCmd.substring(0, commaIndex);
		incCmd.remove(0, commaIndex + 1);
		argIndex++;
	}

	// The rest of the string is the last arg.
	output.args[argIndex] = incCmd;
	output.numArgs = argIndex + 1;

	// Commands that follow are Debug commands, which are handled immediately.

	// Halt command recieved. Requires reset.
	if (output.cmd == "HCF") {
		sendAck();
		while (1) { ; }
	}

	 // Always ACK a RDY.
	 if (output.cmd == "RDY") {
	    sendAck();
	 }

	// Debugging command: Cycle Feed
	if (output.cmd == "CFD") {
		int numCycles = output.args[0].toInt();
		Feed.cycleFeed(numCycles);
		digitalWrite(ledPin, HIGH);
		sendAck();
	}

	// Debugging command: Move Sort Wheel
	if (output.cmd == "MSW") {
		int targetCup = output.args[0].toInt();
		Wheel.moveTo(targetCup);
		digitalWrite(ledPin, HIGH);
		sendAck();
	}

	// Debugging command: Cycle Dispense Arm
	if (output.cmd == "CDA") {
		digitalWrite(ledPin, HIGH);
		SwingArm.write(swingOpen);
		delay(swingTime);
		SwingArm.write(swingHome);
		delay(swingTime);
		digitalWrite(ledPin, LOW);
		sendAck();
	}

	// Debugging command: Test Measurement
	if (output.cmd == "TME") {
		sendAck();

		// Get the current cup and attempt a measurement
		double testMeasurement = measureResistor();
		int thisCup = Wheel.getCurrentPosition();

		// Construct a response
		Command mesCmd;
		mesCmd.cmd = "MES";
		mesCmd.numArgs = 2;
		mesCmd.args[0] = String(thisCup);
		mesCmd.args[1] = String(testMeasurement);

		// Send the measurement data
		sendCommand(mesCmd);

	}

	 if (output.cmd == "RST") {
	    sendAck();
	    _reboot_Teensyduino_();
	 }

	return(output);
}

String parseCmd(Command outCmd) {
	// parses a Command struct into a command string for sendout.

	String output;
	output = outCmd.cmd;
	output += ";";

	for (int i = 0; i < outCmd.numArgs; i++) {
		output += outCmd.args[i];
		output += ",";
	}

	// Delete the last character in the string
	if (outCmd.numArgs > 0) {
		output = output.substring(0, output.length() - 1);
	}

	int verifyValue = output.length() + 1;

	char lenVerify = (char)verifyValue;
	output = lenVerify + output;

	return(output);
}

bool cmdReady() {
	// The first byte of a command will be the number of bytes in the command.
	bool result = ((int) Serial.peek() == (int)Serial.available());
	return(result);
}

void sendCommand(Command sendCmd) {
	String output = parseCmd(sendCmd);

	Serial.println(output);
	Serial.flush();
}

void sendError(String err) {
	// Sends an error to the RPi

	Command errCommand;

	errCommand.cmd = "ERR";
	errCommand.numArgs = 1;
	errCommand.args[0] = err;

	sendCommand(errCommand);
}

void sendDat(String dat) {
	// Sends misc data to the RPi

	Command datCommand;

	datCommand.cmd = "DAT";
	datCommand.numArgs = 1;
	datCommand.args[0] = dat;

	sendCommand(datCommand);
}

void sendReady() {
	Command readyCommand;

	readyCommand.cmd = "RDY";
	readyCommand.numArgs = 0;

	sendCommand(readyCommand);
}

void sendDone() {
	Command doneCommand;

	doneCommand.cmd = "DON";
	doneCommand.numArgs = 0;

	sendCommand(doneCommand);
}

void sendAck() {
	Command ackCommand;

	ackCommand.cmd = "ACK";
	ackCommand.numArgs = 0;

	sendCommand(ackCommand);
}

void clearRegisters() {
	// This function triggers the reset on the shift registers, then latches the empty register.

	digitalWrite(RST0, LOW);	// RESET is Active-low
	delay(1);
	digitalWrite(RST0, HIGH);

	// Going LO-HI-LO ensures a rising edge is seen, then the latch clock is set back to the expected rest state (LOW).
	digitalWrite(LCLK0, LOW);
	delay(1);
	digitalWrite(LCLK0, HIGH);
	delay(1);
	digitalWrite(LCLK0, LOW);
}

double measureResistor() {
	// This function completes a full measurement cycle and returns a resistance in Ohms.
	// 0.0 represents a rejected resistor.
	
	ContactArm.write(contactTouch);
	delay(contactTime);
	
	int medianReading = maxAnalog / 2;
	int cDifference = 99999;
	int bestDifference = 99999;		// Arbitrarily large value here to ensure any reading is superior.
	int bestRange = 0;							// Range 0 is with outputs turned off, a safe fallback in case of failure.
	double bestReading = 0.0;
	double reading = 0.0;
	long readingSums = 0;
		
	// For each range...
	for (int i = 3; i <= 7; i++) {
		
		// Enable the outputs for testing this range and take a measurement.
		ShiftReg.setAll(srState[i]);
		delay(50);							// 5ms maximum operating time for relays, x10 for safety.

		int goodCount = 0;
		int count = 0;
		bool testComplete = false;

		// Try to get 30 good measurements, but give up after 100 attempts.
		while (!testComplete) {
			count++;
			int thisReading = adc->analogRead(RMeas);
			double thisResistance = getResistance(thisReading, i);

			// If the resistance is in an acceptable range, it's good
			if (thisResistance < maxAccepted && thisResistance > 0.5) {
				readingSums = readingSums + thisReading;
				goodCount++;
			}

			if (count > 100) {
				testComplete = true;
			}

			if (goodCount > 30) {
				testComplete = true;
			}
		}

		if (goodCount == 0) {
			reading = 0.0;
		} else {
			// Average the measurements
			reading = (double) readingSums / (double) goodCount;
		}

		cDifference = reading - medianReading;
		cDifference = (cDifference < 0) ? -cDifference : cDifference;	// Absolute value

		// If this is better than the current best, make this the best.
		if (cDifference < bestDifference) {
			bestRange = i;
			bestDifference = cDifference;
			bestReading = reading;
		}
	}

	// Convert the final result to a resistance.
	double result = getResistance(bestReading, bestRange);

	// Return to home position after measurement made.
	ContactArm.write(contactHome);
	delay(contactTime);
	ShiftReg.setAll(srState[0]);
	
	// Return the Ohms value.
	return(result);
}

double getResistance(double measurement, int range) {
	// This function returns a resistance from a given measurement in a given range.

	double result = 0.0;

	// Do not calculate resistances in the cutoff bands.
	if (measurement < adcLCutoff || measurement > adcHCutoff) {
		return(result);
	}

	// Range needs to change into a 0 index (typically it is 1-indexed.)
	range--;

	// 6, 7, and 8 are the current source ranges. 0-5 are Volt Divider ranges
	if (range < 6) {
		// First, convert the reading to volts. (High voltage for dividers)
		double vReading = measurement * (avHigh / maxAnalog);

		// Voltage divider formula solved for R2...
		result = (internalTestResistances[range] * vReading) / (avHigh - vReading);

	} else {
		// Bring the range down to 0 index from 6-8 index
		range = range - 6;

		//Convert the reading to volts. (Low voltage for current sources)
		double vReading = measurement * (avLow / maxAnalog);

		// Current source measurement is simple, V=IR, solving for R gives R=V/I
		result = vReading / internalCurrentSources[range];
	}

	return(result);
}

double getMin(double nominal, double precision) {
	double diff = nominal * precision;
	return(nominal - diff);
}

double getMax(double nominal, double precision) {
	double diff = nominal * precision;
	return(nominal + diff);
}

int getTargetCup(double measurement) {
	// This function checks the measurement against every non-reject cup. If a home is found, that cup number (not index) is returned.
	
	for (int i = 0; i < cupCount; i++) {
		// For each cup, look for a valid home that is not a reject.
		if (Wheel.cups[i].canAccept(measurement) && !(Wheel.cups[i].isReject())) {
			int result = i + 1;
			return(result);	// Return that cup.
		}
	}

	for (int i = 0; i < cupCount; i++) {
		// Find the first available reject cup.
		if (Wheel.cups[i].isReject()) {
			int result = i + 1;
			return(result);	// Return that cup.
		}
	}

	sendError("No Valid Cup Found");
	return(-1);
}