#include "MeasurementState.h"
#include "ValueWidget.h"
#include "LabelWidget.h"
#include "Settings.h"
#include "ADS1115.h"
#include "Adafruit_GFX.h"
#include "AiEsp32RotaryEncoder.h"
#include "PWM.h"
#include "Button.h"
#include <driver/adc.h>


extern GFXcanvas16 s_canvas;
extern AiEsp32RotaryEncoder s_knob;
extern Button s_button;
extern ADS1115 s_adc;
extern Settings s_settings;
extern uint8_t k_disableDacPin;

extern LabelWidget s_modeWidget;
static LabelWidget s_rangeWidget(s_canvas, "");
static ValueWidget s_voltageWidget(s_canvas, 0.f, "V");
static ValueWidget s_currentWidget(s_canvas, 0.f, "A");
static ValueWidget s_powerWidget(s_canvas, 0.f, "W");
static ValueWidget s_trackedWidget(s_canvas, 0.f, "A");
static ValueWidget s_dacWidget(s_canvas, 0.f, "");

static float s_currentRaw = 0.f;
static float s_voltageRaw = 0.f;
static float s_current = 0.f;
static float s_voltage = 0.f;
static bool s_loadOn = false;

ads1115_pga s_rangePgas[Settings::k_rangeCount] = { ADS1115_PGA_SIXTEEN, ADS1115_PGA_EIGHT, ADS1115_PGA_FOUR, ADS1115_PGA_TWO, ADS1115_PGA_ONE };

static uint8_t s_currentRange = 4;
static uint8_t s_nextCurrentRange = 4;
static bool s_isCurrentAutoRanging = true;
static uint8_t s_voltageRange = 4;
static uint8_t s_nextVoltageRange = 4;
static bool s_isVoltageAutoRanging = true;

enum class ADCMux
{
	Current,
	Voltage
};

static ADCMux s_adcMux = ADCMux::Voltage;
static uint32_t s_adcSampleStartedTP = millis();

static float s_temperatureRaw = 0.f;
static float s_temperature = 0.f;

uint8_t getCurrentRange()
{
	return s_currentRange;
}
void setCurrentRange(uint8_t range)
{
	s_isCurrentAutoRanging = false;
	s_nextCurrentRange = range;
}
bool isSwitchingCurrentRange()
{
	return s_currentRange != s_nextCurrentRange;
}
bool isCurrentAutoRanging()
{
	return s_isCurrentAutoRanging;
}
void setCurrentAutoRanging(bool enabled)
{
	s_isCurrentAutoRanging = enabled;
}

uint8_t getVoltageRange()
{
	return s_voltageRange;
}
void setVoltageRange(uint8_t range)
{
	s_isVoltageAutoRanging = false;
	s_nextVoltageRange = range;
}
bool isSwitchingVoltageRange()
{
	return s_voltageRange != s_nextVoltageRange;
}
bool isVoltageAutoRanging()
{
	return s_isVoltageAutoRanging;
}
void setVoltageAutoRanging(bool enabled)
{
	s_isVoltageAutoRanging = enabled;
}

void getCurrentBiasScale(float& bias, float& scale)
{
	bias = s_settings.currentRangeBiases[s_currentRange];
	scale = s_settings.currentRangeScales[s_currentRange];
}
float computeCurrent(float raw)
{
	float bias, scale;
	getCurrentBiasScale(bias, scale);
	/*Serial.print("raw ");
	Serial.print(raw, 4);
	Serial.print(", bias ");
	Serial.print(bias, 4);
	Serial.print(", scale ");
	Serial.print(scale, 4);
	Serial.print(", v ");
	Serial.println((raw + bias) * scale);
	*/
	return (raw + bias) * scale;
}
float getCurrent()
{
	return s_current;
}
float getCurrentRaw()
{
	return s_currentRaw;
}

void getVoltageBiasScale(float& bias, float& scale)
{
	bias = s_settings.voltageRangeBiases[s_voltageRange];
	scale = s_settings.voltageRangeScales[s_voltageRange];
}
float computeVoltage(float raw)
{
	float bias, scale;
	getVoltageBiasScale(bias, scale);
	return (raw + bias) * scale;
}
float getVoltage()
{
	return s_voltage;
}
float getVoltageRaw()
{
	return s_voltageRaw;
}

float computeTemperature(float raw)
{
	return (raw + s_settings.temperatureBias) * s_settings.temperatureScale;
}
float getTemperature()
{
	return s_temperature;
}
float getTemperatureRaw()
{
	return s_temperatureRaw;
}

void readAdcs()
{
	bool voltage, current, temperature;
	readAdcs(voltage, current, temperature);
}

void readAdcs(bool& voltage, bool& current, bool& temperature)
{
	voltage = false;
	current = false;
	temperature = false;

	if (!s_adc.is_sample_in_progress() && millis() >= s_adcSampleStartedTP + 150)
	{
		float val = s_adc.read_sample_float();
		if (s_adcMux == ADCMux::Voltage)
		{
			s_voltageRaw = val;
			s_voltage = computeVoltage(s_voltageRaw);
			voltage = true;

			s_currentRange = s_nextCurrentRange;
			s_adc.set_mux(ADS1115_MUX_GND_AIN2); //switch to current pair
			s_adc.set_pga(s_rangePgas[s_currentRange]);
			s_adc.trigger_sample();
			s_adcMux = ADCMux::Current;
		}
		else
		{
			s_currentRaw = val;
			s_current = computeCurrent(s_currentRaw);
			current = true;

			s_voltageRange = s_nextVoltageRange;
			s_adc.set_mux(ADS1115_MUX_DIFF_AIN0_AIN1); //switch to voltage pair
			s_adc.set_pga(s_rangePgas[s_voltageRange]);
			s_adc.trigger_sample();
			s_adcMux = ADCMux::Voltage;
		}
		s_adcSampleStartedTP = millis();
	}

	s_temperatureRaw = 1.f - (adc1_get_raw(ADC1_CHANNEL_4) / 4096.f);
	s_temperature = (s_temperature * 99.f + computeTemperature(s_temperatureRaw) * 1.f) / 100.f;
	temperature = true;
}

bool isVoltageValid()
{
	return s_voltage > -0.1f;
}


enum class Measurement
{
	ConstantCurrent,
	ConstantPower
};

void processMeasurementState()
{
	readAdcs();

	if (s_knob.currentButtonState() == BUT_RELEASED)
	{
		setVoltageRange((getVoltageRange() + 1) % Settings::k_rangeCount);
		//setCurrentRange((getCurrentRange() + 1) % Settings::k_rangeCount);
	}

	if (s_button.state() == Button::State::RELEASED)
	{
		s_loadOn = !s_loadOn;
		digitalWrite(k_disableDacPin, !s_loadOn);
	}

	//if there is no load, remove any leaking current (usually < 0.001);
	//but don't just set it to zero - I want to see if the leak is too big
	if (!s_loadOn && s_current < 0.002f)
	{
		s_current = 0;
	}

	//Mode
	s_modeWidget.setValue("CC Mode");
	s_modeWidget.update();

	char buf[32];
	sprintf(buf, "(V%d/A%d)", getVoltageRange(), getCurrentRange());
	s_rangeWidget.setValue(buf);
  	s_rangeWidget.setPosition(s_canvas.width() - s_rangeWidget.getWidth() - 2, 11);
	s_rangeWidget.update();

	s_voltageWidget.setTextColor(isVoltageValid() ? 0xFFFF : 0xF000);
	s_voltageWidget.setValue(s_voltage);
	s_voltageWidget.update();

	s_currentWidget.setValue(s_current);
	s_currentWidget.setTextColor(s_loadOn ? 0xF222 : 0xFFFF);
	s_currentWidget.update();

	s_powerWidget.setValue(abs(s_voltage * s_current));
	s_powerWidget.setTextColor(s_loadOn ? 0xF222 : 0xFFFF);
	s_powerWidget.update();

	s_trackedWidget.setValue(s_current);
	s_trackedWidget.update();

	static int32_t desiredCurrent_mAh = 0;
	if (Serial.available() > 0) 
	{
		char ch = Serial.peek();
		if (ch == 'r')
		{
			ESP.restart();
		}
		if (ch == 'f')
		{
			Serial.read();
			static bool fan = false;
			fan = !fan;
			setFanPWM(fan ? 1.f : 0.f);
		}
		//dac = Serial.parseInt();
		//Serial.print("DAC = ");
		//Serial.println(dac);
		int range = Serial.parseInt();
		setVoltageRange(range);
	}
	while (Serial.available()) 
	{
		Serial.read();
	}

	int knobDelta = s_knob.encoderChangedAcc();
	desiredCurrent_mAh += knobDelta;
	desiredCurrent_mAh = std::max(desiredCurrent_mAh, 0);
	desiredCurrent_mAh = std::min(desiredCurrent_mAh, 5 * 1000);

	//setFanPWM(dac / float(k_fanPrecision));

	float scale = 1.f / 0.4586f;
	float bias = -0.0252f;

	//float desiredCurrent = (dac * 3.3f / float(k_dacPrecision) + bias) * scale;

	float desiredCurrent = desiredCurrent_mAh / 1000.f;
	float dac = (desiredCurrent / scale - bias) / 3.3f;
	setDACPWM(dac);

	s_dacWidget.setValue(desiredCurrent);
	s_dacWidget.update();
}

void initMeasurementState()
{
  s_voltageWidget.setTextScale(1);
  s_voltageWidget.setDecimals(3);
  s_voltageWidget.setPosition(0, 30);

  s_currentWidget.setTextScale(1);
  s_currentWidget.setDecimals(3);
  s_currentWidget.setPosition(s_voltageWidget.getX(), s_voltageWidget.getY() + s_voltageWidget.getHeight() + 2);

  s_powerWidget.setTextScale(2);
  s_powerWidget.setPosition(s_currentWidget.getX(), s_currentWidget.getY() + s_currentWidget.getHeight() + 2);

  s_trackedWidget.setTextScale(3);
  s_trackedWidget.setPosition(s_powerWidget.getX(), s_powerWidget.getY() + s_powerWidget.getHeight() + 2);

  s_dacWidget.setTextScale(2);
  s_dacWidget.setDecimals(3);
  s_dacWidget.setPosition(s_trackedWidget.getX(), s_trackedWidget.getY() + s_trackedWidget.getHeight() + 2);
}

void beginMeasurementState()
{
	s_loadOn = false;
	digitalWrite(k_disableDacPin, !s_loadOn);
}
void endMeasurementState()
{

}
