/*
 Name:		LedImagePainter.ino
 Created:	8/5/2020 9:50:12 PM
 Author:	Martin
*/
/*
 Martin Nohr ESP32 LED Image Painter
*/
/*
	BLE code based on Neil Kolban example for IDF: https://github.com/nkolban/esp32-snippets/blob/master/cpp_utils/tests/BLE%20Tests/SampleServer.cpp
*/

#include <ArduinoJson.hpp>
#include <ArduinoJson.h>
//using namespace ARDUINOJSON_NAMESPACE;
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <BLE2902.h>
#include <ringbufcpp.h>
#include "LedImagePainter.h"

// rotary switch
#define BTNPUSH 27
#define BTNA 12
#define BTNB 14
#define FRAMEBUTTON 26

#define MAX_KEY_BUF 10
RingBufCPP<int, MAX_KEY_BUF> btnBuf;
enum BUTTONS { BTN_RIGHT, BTN_LEFT, BTN_SELECT, BTN_LONG, BTN_NONE };
// for debugging missed buttons
volatile int nButtonDowns;
volatile int nButtonUps;
volatile bool bButtonTimerRunning;
volatile bool bButtonArmed = true;	// must be reset after reading a long press, it stops the noise on button release

// interrupt handlers
void IRAM_ATTR IntBtnCenter()
{
	// went low, if timer not started, start it
	if (!bButtonTimerRunning && bButtonArmed) {
		//esp_timer_stop(oneshot_LONGPRESS_timer);	// just in case
		esp_timer_start_once(oneshot_LONGPRESS_timer, 400 * 1000);
		bButtonTimerRunning = true;
	}
}

void IRAM_ATTR oneshot_LONGPRESS_timer_callback(void* arg)
{
	int btn;
	// if the button is down, it must be a long press
	btn = digitalRead(BTNPUSH) ? BTN_SELECT : BTN_LONG;
	if (bButtonArmed) {
		btnBuf.add(btn);
		//Serial.println("btn: " + String(btn));
		if (btn == BTN_LONG) {
			bButtonArmed = false;
		}
	}
	bButtonTimerRunning = false;
}

// state table for the rotary encoder
#define T true
#define F false
#define DIRECTIONS 2
#define MAXSTATE 4
#define CONTACTS 2
bool stateTest[DIRECTIONS][MAXSTATE][CONTACTS] =
{
	{{T,F},{F,F},{F,T},{T,T}},
	{{F,T},{F,F},{T,F},{T,T}}
};
#define A 0
#define B 1
#define ROTARY_RETRIES 10
void IRAM_ATTR IntBtnAB()
{
	static bool forward;
	static int state = 0;
	static int tries;
	static bool lastValA = true;
	static bool lastValB = true;
	bool valA = digitalRead(BTNA);
	bool valB = digitalRead(BTNB);
	//Serial.println("A:" + String(valA) + " B:" + String(valB));
	//Serial.println("start state: " + String(state));
	//Serial.println("forward: " + String(forward));
	if (valA == lastValA && valB == lastValB)
		return;
	if (state == 0) {
		// starting
		// see if one of the first tests is correct, then go to state 1
		if (stateTest[0][state][A] == valA && stateTest[0][state][B] == valB) {
			//Serial.println("down");
			forward = false;
			tries = ROTARY_RETRIES;
			++state;
		}
		else if (stateTest[1][state][A] == valA && stateTest[1][state][B] == valB) {
			//Serial.println("up");
			forward = true;
			tries = ROTARY_RETRIES;
			++state;
		}
	}
	else {
		// check if we can advance
		if (stateTest[forward ? 1 : 0][state][A] == valA && stateTest[forward ? 1 : 0][state][B] == valB) {
			tries = ROTARY_RETRIES;
			++state;
		}
		else {
			//state = 0;
		}
	}
	//Serial.println("end state: " + String(state));
	//Serial.println("forward: " + String(forward));
	if (state == MAXSTATE) {
		// we're done
		//Serial.println(String(forward ? "+" : "-"));
		state = 0;
		int btn = forward ? BTN_RIGHT : BTN_LEFT;
		//Serial.println("add: " + String(btn));
		btnBuf.add(btn);
	}
	else if ((tries-- <= 0 && state > 0) || (valA == true && valB == true)) {
		// something failed, start over
		//Serial.println("failed");
		//int btn = BTN_NONE;
		//btnBuf.add(btn);
		state = 0;
	}
	lastValA = valA;
	lastValB = valB;
}

//static const char* TAG = "lightwand";
//esp_timer_cb_t oneshot_timer_callback(void* arg)
void IRAM_ATTR oneshot_LED_timer_callback(void* arg)
{
	bStripWaiting = false;
	//int64_t time_since_boot = esp_timer_get_time();
	//Serial.println("in isr");
	//ESP_LOGI(TAG, "One-shot timer called, time since boot: %lld us", time_since_boot);
}

void IRAM_ATTR oneshot_BTN_timer_callback(void* arg)
{
	bButtonWait = false;
}

class MyServerCallbacks : public BLEServerCallbacks {
	void onConnect(BLEServer* pServer) {
		BLEDeviceConnected = true;
		//OLED->clear();
		//WriteMessage("BLE connected");
		//OLED->clear();
		//DisplayCurrentFile();
		//Serial.println("BLE connected");
	};

	void onDisconnect(BLEServer* pServer) {
		BLEDeviceConnected = false;
		//OLED->clear();
		//WriteMessage("BLE disconnected");
		//OLED->clear();
		//DisplayCurrentFile();
		//Serial.println("BLE disconnected");
	}
};

class MyCharacteristicCallbacks : public BLECharacteristicCallbacks {
	void onWrite(BLECharacteristic* pCharacteristic) {
		BLEUUID uuid = pCharacteristic->getUUID();
		//Serial.println("UUID:" + String(uuid.toString().c_str()));
		std::string value = pCharacteristic->getValue();
		String stmp = value.c_str();

		if (value.length() > 0) {
			//Serial.println("*********");
			//Serial.print("New value: ");
			//for (int i = 0; i < value.length(); i++) {
			//	Serial.print(value[i]);
			//}
			//Serial.println();
			//Serial.println("*********");
			// see if this a UUID we can change
			if (uuid.equals(BLEUUID(CHARACTERISTIC_UUID_RUN))) {
				sBLECommand = value;
			}
			else if (uuid.equals(BLEUUID(CHARACTERISTIC_UUID_WANDSETTINGS))) {
				//Serial.println(value.c_str());
				StaticJsonDocument<200> doc;
				const char* json = value.c_str();

				// Deserialize the JSON document
				DeserializationError error = deserializeJson(doc, json);

				// Test if parsing succeeds.
				if (error) {
					//Serial.print("deserializeJson() failed: ");
					//Serial.println(error.c_str());
					return;
				}
				JsonObject object = doc.as<JsonObject>();
				// see if brightness is there
				JsonVariant jv = doc.getMember("bright");
				if (!jv.isNull()) {
					nStripBrightness = jv.as<int>();
				}
				// see if repeat
				jv = doc.getMember("repeatcount");
				if (!jv.isNull()) {
					repeatCount = jv.as<int>();
				}
				// see if repeat delay
				jv = doc.getMember("repeatdelay");
				if (!jv.isNull()) {
					repeatDelay = jv.as<int>();
				}
				// change the current file
				jv = doc.getMember("current");
				if (!jv.isNull()) {
					String fn = jv.as<char*>();
					// lets search for the file
					for (int ix = 0; ix < NumberOfFiles; ++ix) {
						if (FileNames[ix].equals(fn)) {
							CurrentFileIndex = ix;
							DisplayCurrentFile();
							break;
						}
					}
				}
			}
			//else if (uuid.equals(BLEUUID(CHARACTERISTIC_UUID_LEDBRIGHT))) {
			//	nStripBrightness = stmp.toInt();
			//	nStripBrightness = constrain(nStripBrightness, 1, 100);
			//}
			//else if (uuid.equals(BLEUUID(CHARACTERISTIC_UUID_BUILTIN))) {
			//	stmp.toUpperCase();
			//	bool newval = stmp.equals("TRUE") ? true : false;
			//	//Serial.println("newval:" + String(newval ? "true" : "false"));
			//	if (newval != bShowBuiltInTests) {
			//		//Serial.println("builtin:" + String(bShowBuiltInTests ? "true" : "false"));
			//		ToggleFilesBuiltin(NULL);
			//		//Serial.println("builtin:" + String(bShowBuiltInTests ? "true" : "false"));
			//		OLED->clear();
			//		DisplayCurrentFile();
			//	}
			//}
			//else if (uuid.equals(BLEUUID(CHARACTERISTIC_UUID_2LEDSTRIPS))) {
			//	stmp.toUpperCase();
			//	bSecondStrip = stmp.compareTo("TRUE") == 0 ? true : false;
			//}
			else if (uuid.equals(BLEUUID(CHARACTERISTIC_UUID_FILEINFO))) {
				// change the current file here, maybe we search for it?
			}
			//else if (uuid.equals(BLEUUID(CHARACTERISTIC_UUID_FRAMETIME))) {
			//	frameHold = stmp.toInt();
			//}
			//else if (uuid.equals(BLEUUID(CHARACTERISTIC_UUID_STARTDELAY))) {
			//	startDelay = stmp.toInt();
			//}
			UpdateBLE(false);
		}
	}
};

void EnableBLE()
{
	BLEDevice::init("MN LED Image Painter");
	BLEServer* pServer = BLEDevice::createServer();
	pServer->setCallbacks(new MyServerCallbacks());

	BLEService* pServiceDevInfo = pServer->createService(BLEUUID((uint16_t)0x180a));
	BLECharacteristic* pCharacteristicDevInfo = pServiceDevInfo->createCharacteristic(
		BLEUUID((uint16_t)0x2a29),
		BLECharacteristic::PROPERTY_READ
	);
	pCharacteristicDevInfo->setValue("NOHR PHOTO");

	pCharacteristicDevInfo = pServiceDevInfo->createCharacteristic(
		BLEUUID((uint16_t)0x2a24),	// model
		BLECharacteristic::PROPERTY_READ
	);
	pCharacteristicDevInfo->setValue("LED Image Painter Small Display");

	pCharacteristicDevInfo = pServiceDevInfo->createCharacteristic(
		BLEUUID((uint16_t)0x2a28),	// software version
		BLECharacteristic::PROPERTY_READ
	);
	pCharacteristicDevInfo->setValue("1.1");

	pCharacteristicDevInfo = pServiceDevInfo->createCharacteristic(
		BLEUUID((uint16_t)0x2a27),	// hardware version
		BLECharacteristic::PROPERTY_READ
	);
	pCharacteristicDevInfo->setValue("1.0");

	BLEService* pService = pServer->createService(SERVICE_UUID);
	// filepath
	pCharacteristicFileInfo = pService->createCharacteristic(
		CHARACTERISTIC_UUID_FILEINFO,
		BLECharacteristic::PROPERTY_READ |
		BLECharacteristic::PROPERTY_WRITE
	);
	// Create a BLE Descriptor
	//pCharacteristicFilename->addDescriptor(new BLE2902());

	// Wand settins
	pCharacteristicWandSettings = pService->createCharacteristic(
		CHARACTERISTIC_UUID_WANDSETTINGS,
		BLECharacteristic::PROPERTY_READ |
		BLECharacteristic::PROPERTY_WRITE
	);

	// run command
	pCharacteristicRun = pService->createCharacteristic(
		CHARACTERISTIC_UUID_RUN,
		BLECharacteristic::PROPERTY_READ |
		BLECharacteristic::PROPERTY_WRITE
	);

	// all the filenames
	pCharacteristicFileList = pService->createCharacteristic(
		CHARACTERISTIC_UUID_FILELIST,
		BLECharacteristic::PROPERTY_READ
	);

	// add anybody that can be changed or can call us with something to do
	MyCharacteristicCallbacks* pCallBacks = new MyCharacteristicCallbacks();
	pCharacteristicRun->setCallbacks(pCallBacks);
	pCharacteristicWandSettings->setCallbacks(pCallBacks);
	pCharacteristicFileInfo->setCallbacks(pCallBacks);
	UpdateBLE(false);
	pService->start();
	pServiceDevInfo->start();
	BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
	pAdvertising->addServiceUUID(SERVICE_UUID);
	pAdvertising->addServiceUUID(BLEUUID((uint16_t)0x180a));
	pAdvertising->setScanResponse(true);
	pAdvertising->setMinPreferred(0x06);  // functions that help with iPhone connections issue
	pAdvertising->setMinPreferred(0x12);
	BLEDevice::startAdvertising();
}

void setup()
{
	pinMode(LED, OUTPUT);
	digitalWrite(LED, HIGH);
	pinMode(BTNPUSH, INPUT_PULLUP);
	pinMode(BTNA, INPUT_PULLUP);
	pinMode(BTNB, INPUT_PULLUP);
	pinMode(FRAMEBUTTON, INPUT_PULLUP);
	oneshot_LED_timer_args = {
				oneshot_LED_timer_callback,
				/* argument specified here will be passed to timer callback function */
				(void*)TID_LED,
				ESP_TIMER_TASK,
				"one-shotLED"
	};
	esp_timer_create(&oneshot_LED_timer_args, &oneshot_LED_timer);
	//oneshot_BTN_timer_args = {
	//		oneshot_BTN_timer_callback,
	//		/* argument specified here will be passed to timer callback function */
	//		(void*)TID_BTN,
	//		ESP_TIMER_TASK,
	//		"one-shotBTN"
	//};
	//esp_timer_create(&oneshot_BTN_timer_args, &oneshot_BTN_timer);
	// the long press timer
	oneshot_LONGPRESS_timer_args = {
			oneshot_LONGPRESS_timer_callback,
			/* argument specified here will be passed to timer callback function */
			(void*)TID_LONGPRESS,
			ESP_TIMER_TASK,
			"one-shotLONGPRESS"
	};
	esp_timer_create(&oneshot_LONGPRESS_timer_args, &oneshot_LONGPRESS_timer);

	attachInterrupt(BTNPUSH, IntBtnCenter, FALLING);
	attachInterrupt(BTNA, IntBtnAB, CHANGE);
	attachInterrupt(BTNB, IntBtnAB, CHANGE);
	Heltec.begin(true /*DisplayEnable Enable*/, false /*LoRa Enable*/, true /*Serial Enable*/);
	delay(100);
	digitalWrite(LED, LOW);
	int width = OLED->width();
	int height = OLED->height();
	OLED->clear();
	OLED->drawRect(0, 0, width - 1, height - 1);
	OLED->setFont(ArialMT_Plain_24);
	OLED->drawString(2, 2, "MN Painter");
	OLED->setFont(ArialMT_Plain_16);
	OLED->drawString(4, 30, "Version 2.04");
	OLED->setFont(ArialMT_Plain_10);
	OLED->drawString(4, 48, __DATE__);
	OLED->display();
	OLED->setFont(ArialMT_Plain_10);
	charHeight = 13;
	EEPROM.begin(1024);
	if (!SaveSettings(false, true) || digitalRead(BTNPUSH) == LOW) {
		SaveSettings(true, bAutoLoadSettings);
	}
	setupSDcard();
	menuPtr = new MenuInfo;
	MenuStack.push(menuPtr);
	MenuStack.peek()->menu = MainMenu;
	MenuStack.peek()->index = 0;
	MenuStack.peek()->offset = 0;
	//const int ledPin = 16;  // 16 corresponds to GPIO16

	//// configure LED PWM functionalitites
	//ledcSetup(ledChannel, freq, resolution);
	//// attach the channel to the GPIO to be controlled
	//ledcAttachPin(WandPin, ledChannel);
	//ledcWrite(ledChannel, 200);
	FastLED.addLeds<NEOPIXEL, DATA_PIN1>(leds, 0, NUM_LEDS);
	//FastLED.addLeds<NEOPIXEL, DATA_PIN2>(leds, 0, NUM_LEDS);	// to test parallel second strip
	//if (bSecondStrip)
	// create the second led controller
	FastLED.addLeds<NEOPIXEL, DATA_PIN2>(leds, NUM_LEDS, NUM_LEDS);
	//FastLED.setTemperature(whiteBalance);
	FastLED.setTemperature(CRGB(whiteBalance.r, whiteBalance.g, whiteBalance.b));
	FastLED.setBrightness(nStripBrightness);
	// Turn the LED on, then pause
	SetPixel(0, CRGB::Red);
	SetPixel(1, CRGB::Red);
	SetPixel(4, CRGB::Green);
	SetPixel(5, CRGB::Green);
	SetPixel(8, CRGB::Blue);
	SetPixel(9, CRGB::Blue);
	SetPixel(12, CRGB::White);
	SetPixel(13, CRGB::White);
	SetPixel(NUM_LEDS - 0, CRGB::Red);
	SetPixel(NUM_LEDS - 1, CRGB::Red);
	SetPixel(NUM_LEDS - 4, CRGB::Green);
	SetPixel(NUM_LEDS - 5, CRGB::Green);
	SetPixel(NUM_LEDS - 8, CRGB::Blue);
	SetPixel(NUM_LEDS - 9, CRGB::Blue);
	SetPixel(NUM_LEDS - 12, CRGB::White);
	SetPixel(NUM_LEDS - 13, CRGB::White);
	SetPixel(0 + NUM_LEDS, CRGB::Red);
	SetPixel(1 + NUM_LEDS, CRGB::Red);
	SetPixel(4 + NUM_LEDS, CRGB::Green);
	SetPixel(5 + NUM_LEDS, CRGB::Green);
	SetPixel(8 + NUM_LEDS, CRGB::Blue);
	SetPixel(9 + NUM_LEDS, CRGB::Blue);
	SetPixel(12 + NUM_LEDS, CRGB::White);
	SetPixel(13 + NUM_LEDS, CRGB::White);
	for (int ix = 0; ix < 255; ix += 5) {
		FastLED.setBrightness(ix);
		FastLED.show();
		delayMicroseconds(50);
	}
	for (int ix = 255; ix >= 0; ix -= 5) {
		FastLED.setBrightness(ix);
		FastLED.show();
		delayMicroseconds(50);
	}
	delayMicroseconds(50);
	FastLED.clear(true);
	delayMicroseconds(50);
	FastLED.setBrightness(nStripBrightness);
	delay(50);
	// Now turn the LED off
	FastLED.clear(true);
	delayMicroseconds(50);
	// run a white dot up the display and back
	for (int ix = 0; ix < STRIPLENGTH; ++ix) {
		SetPixel(ix, CRGB::White);
		if (ix)
			SetPixel(ix - 1, CRGB::Black);
		FastLED.show();
		delayMicroseconds(50);
	}
	for (int ix = STRIPLENGTH - 1; ix >= 0; --ix) {
		SetPixel(ix, CRGB::White);
		if (ix)
			SetPixel(ix + 1, CRGB::Black);
		FastLED.show();
		delayMicroseconds(50);
	}
	FastLED.clear(true);
	delayMicroseconds(50);
	delay(100);
	OLED->clear();
	if (!bSdCardValid) {
		DisplayCurrentFile();
		delay(2000);
		ToggleFilesBuiltin(NULL);
	}
	DisplayCurrentFile();
	
	if (bEnableBLE) {
		EnableBLE();
	}
}

void loop()
{
	static bool didsomething = false;
	bool lastStrip = bSecondStrip;
	bool bLastEnableBLE = bEnableBLE;
	//int lastDisplayBrightness = displayBrightness;
	didsomething = bSettingsMode ? HandleMenus() : HandleRunMode();
	// special handling for things that might have changed
	//if (lastDisplayBrightness != displayBrightness) {
	//	OLED->setBrightness(displayBrightness);
	//}
	//if (lastStrip != bSecondStrip) {
		//******** this crashes
		//if (bSecondStrip)
		//	FastLED.addLeds<NEOPIXEL, DATA_PIN2>(leds, NUM_LEDS, NUM_LEDS);
		//else
		//	FastLED.addLeds<NEOPIXEL, DATA_PIN2>(leds, NUM_LEDS, 0);
	//}
	// wait for no keys
	if (didsomething) {
		//Serial.println("calling wait for none");
		UpdateBLE(false);
		didsomething = false;
		// see if BLE enabled
		if (bEnableBLE != bLastEnableBLE) {
			if (bEnableBLE) {
				EnableBLE();
			}
			else {
				// shutdown BLE
				BLEDeviceConnected = false;
				// TODO: is there anything else we need to do here?
			}
		}
		delay(1);
	}
	// disconnecting
	if (!BLEDeviceConnected && oldBLEDeviceConnected) {
		delay(500); // give the bluetooth stack the chance to get things ready
		BLEDevice::startAdvertising();
		//Serial.println("start advertising");
		oldBLEDeviceConnected = BLEDeviceConnected;
	}
	// connecting
	if (BLEDeviceConnected && !oldBLEDeviceConnected) {
		// do stuff here on connecting
		oldBLEDeviceConnected = BLEDeviceConnected;
		UpdateBLE(false);
	}
}

void UpdateBLE(bool bProgressOnly)
{
	// update the settings, except for progress percent which is done in ShowProgress.
	if (BLEDeviceConnected) {
		String js;
		// running information and commands
		DynamicJsonDocument runinfo(256);
		runinfo["running"] = bIsRunning;
		runinfo["progress"] = nProgress;
		runinfo["repeatsleft"] = nRepeatsLeft;
		js = "";
		serializeJson(runinfo, js);
		pCharacteristicRun->setValue(js.c_str());
		if (!bProgressOnly) {
			// file information
			DynamicJsonDocument filesdoc(1024);
			filesdoc["builtin"] = bShowBuiltInTests;
			filesdoc["path"] = currentFolder.c_str();
			filesdoc["current"] = FileNames[CurrentFileIndex].c_str();
			js = "";
			serializeJson(filesdoc, js);
			pCharacteristicFileInfo->setValue(js.c_str());
			// settings
			DynamicJsonDocument wsdoc(1024);
			wsdoc["secondstrip"] = bSecondStrip;
			wsdoc["bright"] = nStripBrightness;
			wsdoc["framehold"] = frameHold;
			wsdoc["framebutton"] = nFramePulseCount;
			wsdoc["startdelay"] = startDelay;
			wsdoc["repeatdelay"] = repeatDelay;
			wsdoc["repeatcount"] = repeatCount;
			wsdoc["gamma"] = bGammaCorrection;
			wsdoc["reverse"] = bReverseImage;
			wsdoc["upsidedown"] = bUpsideDown;
			wsdoc["mirror"] = bMirrorPlayImage;
			wsdoc["chain"] = bChainFiles;
			wsdoc["chainrepeats"] = nChainRepeats;
			wsdoc["scaley"] = bScaleHeight;
			js = "";
			serializeJson(wsdoc, js);
			pCharacteristicWandSettings->setValue(js.c_str());
			// file list
			DynamicJsonDocument filelist(512);
			filelist["builtin"] = bShowBuiltInTests;
			filelist["path"] = currentFolder.c_str();
			filelist["count"] = NumberOfFiles;
			filelist["ix"] = CurrentFileIndex;
			JsonArray data = filelist.createNestedArray("files");
			for (int ix = 0; ix < NumberOfFiles; ++ix) {
				data.add(FileNames[ix].c_str());
			}
			js = "";
			serializeJson(filelist, js);
			pCharacteristicFileList->setValue(js.c_str());
		}
	}
}

bool RunMenus(int button)
{
	// see if we got a menu match
	bool gotmatch = false;
	int menuix = 0;
	MenuInfo* oldMenu;
	for (int ix = 0; !gotmatch && MenuStack.peek()->menu[ix].op != eTerminate; ++ix) {
		// see if this is one is valid
		if (!MenuStack.peek()->menu[ix].valid) {
			continue;
		}
		//Serial.println("menu button: " + String(button));
		if (button == BTN_SELECT && menuix == MenuStack.peek()->index) {
			//Serial.println("got match " + String(menuix) + " " + String(MenuStack.peek()->index));
			gotmatch = true;
			//Serial.println("clicked on menu");
			// got one, service it
			switch (MenuStack.peek()->menu[ix].op) {
			case eText:
			case eTextInt:
			case eTextCurrentFile:
			case eBool:
				if (MenuStack.peek()->menu[ix].function) {
					//Serial.println(ix);
					(*MenuStack.peek()->menu[ix].function)(&MenuStack.peek()->menu[ix]);
					bMenuChanged = true;
				}
				break;
			case eMenu:
				oldMenu = MenuStack.peek();
				MenuStack.push(new MenuInfo);
				MenuStack.peek()->menu = (MenuItem*)(oldMenu->menu[ix].value);
				bMenuChanged = true;
				MenuStack.peek()->index = 0;
				MenuStack.peek()->offset = 0;
				//Serial.println("change menu");
				break;
			case eBuiltinOptions: // find it in builtins
				if (BuiltInFiles[CurrentFileIndex].menu != NULL) {
					MenuStack.peek()->index = MenuStack.peek()->index;
					MenuStack.push(new MenuInfo);
					MenuStack.peek()->menu = (MenuItem*)(BuiltInFiles[CurrentFileIndex].menu);
					MenuStack.peek()->index = 0;
					MenuStack.peek()->offset = 0;
				}
				else {
					WriteMessage("No settings available for:\n" + String(BuiltInFiles[CurrentFileIndex].text));
				}
				bMenuChanged = true;
				break;
			case eExit: // go back a level
				if (MenuStack.count() > 1) {
					delete MenuStack.peek();
					MenuStack.pop();
					bMenuChanged = true;
				}
				break;
			case eReboot:
				WriteMessage("Rebooting in 2 seconds\nHold button for factory reset", false, 2000);
				ESP.restart();
				break;
			}
		}
		++menuix;
	}
	// if no match, and we are in a submenu, go back one level
	if (!bMenuChanged && MenuStack.count() > 1) {
		bMenuChanged = true;
		menuPtr = MenuStack.pop();
		delete menuPtr;
	}
}

// display the menu
// if MenuStack.peek()->index is > 5, then shift the lines up by enough to display them
// remember that we only have room for 5 lines
void ShowMenu(struct MenuItem* menu)
{
	MenuStack.peek()->menucount = 0;
	//offsetLines = max(0, MenuStack.peek()->index - 4);
	//Serial.println("offset: " + String(offsetLines));
	int y = 0;
	int x = 0;
	char line[100];
	bool skip = false;
	//Serial.println("enter showmenu");
	// loop through the menu
	for (; menu->op != eTerminate; ++menu) {
		menu->valid = false;
		switch (menu->op) {
		case eIfEqual:
			// skip the next one if match, only booleans are handled so far
			skip = *(bool*)menu->value != (menu->min ? true : false);
			//Serial.println("ifequal test: skip: " + String(skip));
			break;
		case eElse:
			skip = !skip;
			break;
		case eEndif:
			skip = false;
			break;
		}
		if (skip) {
			menu->valid = false;
			continue;
		}
		char line[100];
		// only displayable menu items should be in this switch
		line[0] = '\0';
		switch (menu->op) {
		case eTextInt:
		case eText:
		case eTextCurrentFile:
			menu->valid = true;
			if (menu->value) {
				if (menu->op == eText)
					sprintf(line, menu->text, (char*)menu->value);
				else if (menu->op == eTextInt) {
					int val = *(int*)menu->value;
					if (menu->decimals == 0) {
						sprintf(line, menu->text, val);
					}
					else {
						sprintf(line, menu->text, val / 10, val % 10);
					}
				}
				//Serial.println("menu text1: " + String(line));
			}
			else {
				if (menu->op == eTextCurrentFile) {
					sprintf(line, menu->text, MakeIPCFilename(FileNames[CurrentFileIndex], false).c_str());
					//Serial.println("menu text2: " + String(line));
				}
				else {
					strcpy(line, menu->text);
					//Serial.println("menu text3: " + String(line));
				}
			}
			// next line
			++y;
			break;
		case eBool:
			menu->valid = true;
			if (menu->value) {
				sprintf(line, menu->text, *(bool*)menu->value ? menu->on : menu->off);
				//Serial.println("bool line: " + String(line));
			}
			else {
				strcpy(line, menu->text);
			}
			// increment displayable lines
			++y;
			break;
		case eBuiltinOptions:
			// for builtins only show if available
			if (BuiltInFiles[CurrentFileIndex].menu != NULL) {
				menu->valid = true;
				sprintf(line, menu->text, BuiltInFiles[CurrentFileIndex].text);
				++y;
			}
			break;
		case eMenu:
		case eExit:
		case eReboot:
			menu->valid = true;
			strcpy(line, (String((menu->op == eReboot) ? "" : "+") + menu->text).c_str());
			++y;
			//Serial.println("menu text4: " + String(line));
			break;
		}
		if (strlen(line) && y >= MenuStack.peek()->offset) {
			DisplayMenuLine(y - 1, y - 1 - MenuStack.peek()->offset, line);
		}
	}
	//Serial.println("menu: " + String(offsetLines) + ":" + String(y) + " active: " + String(MenuStack.peek()->index));
	MenuStack.peek()->menucount = y;
	// blank the rest of the lines
	for (int ix = y; ix < 5; ++ix) {
		DisplayLine(ix, "");
	}
	// show line if menu has been scrolled
	if (MenuStack.peek()->offset > 0)
		OLED->drawLine(0, 0, 5, 0);
	// show bottom line if last line is showing
	if (MenuStack.peek()->offset + 4 < MenuStack.peek()->menucount - 1)
		OLED->drawLine(0, OLED->getHeight() - 1, 5, OLED->getHeight() - 1);
	OLED->display();
}

// switch between SD and built-ins
void ToggleFilesBuiltin(MenuItem* menu)
{
	//Serial.println("toggle builtin");
	bool lastval = bShowBuiltInTests;
	int oldIndex = CurrentFileIndex;
	String oldFolder = currentFolder;
	if (menu != NULL) {
		ToggleBool(menu);
	}
	else {
		bShowBuiltInTests = !bShowBuiltInTests;
	}
	if (lastval != bShowBuiltInTests) {
		if (bShowBuiltInTests) {
			CurrentFileIndex = 0;
			NumberOfFiles = 0;
			for (NumberOfFiles = 0; NumberOfFiles < sizeof(BuiltInFiles) / sizeof(*BuiltInFiles); ++NumberOfFiles) {
				// add each one
				FileNames[NumberOfFiles] = String(BuiltInFiles[NumberOfFiles].text);
			}
			currentFolder = "";
		}
		else {
			// read the SD
			currentFolder = lastFolder;
			GetFileNamesFromSD(currentFolder);
		}
	}
	// restore indexes
	CurrentFileIndex = lastFileIndex;
	lastFileIndex = oldIndex;
	currentFolder = lastFolder;
	lastFolder = oldFolder;
}

// toggle a boolean value
void ToggleBool(MenuItem* menu)
{
	//Serial.println("toggle bool");
	bool* pb = (bool*)menu->value;
	*pb = !*pb;
}

// get integer values
void GetIntegerValue(MenuItem* menu)
{
	// -1 means to reset to original
	int stepSize = 1;
	int originalValue = *(int*)menu->value;
	//Serial.println("int: " + String(menu->text) + String(*(int*)menu->value));
	char line[50];
	int button = BTN_NONE;
	bool done = false;
	OLED->clear();
	DisplayLine(1, "Range: " + String(menu->min) + " to " + String(menu->max));
	DisplayLine(3, "Long Press to Accept");
	int oldVal = *(int*)menu->value;
	if (menu->change != NULL) {
		(*menu->change)(menu, 1);
	}
	do {
		//Serial.println("button: " + String(button));
		switch (button) {
		case BTN_LEFT:
			if (stepSize != -1)
				*(int*)menu->value -= stepSize;
			break;
		case BTN_RIGHT:
			if (stepSize != -1)
				*(int*)menu->value += stepSize;
			break;
		case BTN_SELECT:
			if (stepSize == -1) {
				stepSize = 1;
			}
			else {
				stepSize *= 10;
			}
			if (stepSize > (menu->max / 10)) {
				stepSize = -1;
			}
			break;
		case BTN_LONG:
			if (stepSize == -1) {
				*(int*)menu->value = originalValue;
				stepSize = 1;
			}
			else {
				done = true;
			}
			break;
		}
		// make sure within limits
		*(int*)menu->value = constrain(*(int*)menu->value, menu->min, menu->max);
		// show slider bar
		OLEDDISPLAY_COLOR oldColor = OLED->getColor();
		OLED->setColor(OLEDDISPLAY_COLOR::BLACK);
		OLED->fillRect(0, 30, OLED->width() - 1, 6);
		OLED->setColor(oldColor);
		OLED->drawProgressBar(0, 30, OLED->width() - 1, 6, map(*(int*)menu->value, menu->min, menu->max, 0, 100));
		if (menu->decimals == 0) {
			sprintf(line, menu->text, *(int*)menu->value);
		}
		else {
			sprintf(line, menu->text, *(int*)menu->value / 10, *(int*)menu->value % 10);
		}
		DisplayLine(0, line);
		DisplayLine(4, stepSize == -1 ? "Reset: long press (Click +)" : "step: " + String(stepSize) + " (Click +)");
		if (menu->change != NULL && oldVal != *(int*)menu->value) {
			(*menu->change)(menu, 0);
			oldVal = *(int*)menu->value;
		}
		while (!done && (button = ReadButton()) == BTN_NONE) {
			delay(1);
		}
	} while (!done);
	if (menu->change != NULL) {
		(*menu->change)(menu, -1);
	}
}

void UpdateStripBrightness(MenuItem* menu, int flag)
{
	switch (flag) {
	case 1:		// first time
		for (int ix = 0; ix < 64; ++ix) {
			SetPixel(ix, CRGB::White);
		}
		FastLED.show();
		break;
	case 0:		// every change
		FastLED.setBrightness(*(int*)menu->value);
		FastLED.show();
		break;
	case -1:	// last time
		FastLED.clear(true);
		break;
	}
}

void UpdateStripWhiteBalanceR(MenuItem* menu, int flag)
{
	switch (flag) {
	case 1:		// first time
		for (int ix = 0; ix < 64; ++ix) {
			SetPixel(ix, CRGB::White);
		}
		FastLED.show();
		break;
	case 0:		// every change
		FastLED.setTemperature(CRGB(*(int*)menu->value, whiteBalance.g, whiteBalance.b));
		FastLED.show();
		break;
	case -1:	// last time
		FastLED.clear(true);
		break;
	}
}

void UpdateStripWhiteBalanceG(MenuItem* menu, int flag)
{
	switch (flag) {
	case 1:		// first time
		for (int ix = 0; ix < 64; ++ix) {
			SetPixel(ix, CRGB::White);
		}
		FastLED.show();
		break;
	case 0:		// every change
		FastLED.setTemperature(CRGB(whiteBalance.r, *(int*)menu->value, whiteBalance.b));
		FastLED.show();
		break;
	case -1:	// last time
		FastLED.clear(true);
		break;
	}
}

void UpdateStripWhiteBalanceB(MenuItem* menu, int flag)
{
	switch (flag) {
	case 1:		// first time
		for (int ix = 0; ix < 64; ++ix) {
			SetPixel(ix, CRGB::White);
		}
		FastLED.show();
		break;
	case 0:		// every change
		FastLED.setTemperature(CRGB(whiteBalance.r, whiteBalance.g, *(int*)menu->value));
		FastLED.show();
		break;
	case -1:	// last time
		FastLED.clear(true);
		break;
	}
}

void UpdateOledBrightness(MenuItem* menu, int flag)
{
	OLED->setBrightness(map(*(int*)menu->value, 0, 100, 0, 255));
}

// handle the menus
bool HandleMenus()
{
	if (bMenuChanged) {
		ShowMenu(MenuStack.peek()->menu);
		//ShowGo();
		bMenuChanged = false;
	}
	bool didsomething = true;
	int button = ReadButton();
	int lastOffset = MenuStack.peek()->offset;
	int lastMenu = MenuStack.peek()->index;
	switch (button) {
	case BTN_SELECT:
		RunMenus(button);
		bMenuChanged = true;
		break;
	case BTN_RIGHT:
		if (bAllowMenuWrap || MenuStack.peek()->index < MenuStack.peek()->menucount - 1) {
			++MenuStack.peek()->index;
		}
		if (MenuStack.peek()->index >= MenuStack.peek()->menucount) {
			MenuStack.peek()->index = 0;
		}
		// see if we need to scroll the menu
		if (MenuStack.peek()->index - MenuStack.peek()->offset > 4) {
			if (MenuStack.peek()->offset < MenuStack.peek()->menucount - 5) {
				++MenuStack.peek()->offset;
			}
		}
		break;
	case BTN_LEFT:
		if (bAllowMenuWrap || MenuStack.peek()->index > 0) {
			--MenuStack.peek()->index;
		}
		if (MenuStack.peek()->index < 0) {
			MenuStack.peek()->index = MenuStack.peek()->menucount - 1;
		}
		// see if we need to adjust the offset
		if (MenuStack.peek()->offset && MenuStack.peek()->index < MenuStack.peek()->offset) {
			--MenuStack.peek()->offset;
		}
		break;
	case BTN_LONG:
		OLED->clear();
		bSettingsMode = false;
		DisplayCurrentFile();
		bMenuChanged = true;
		break;
	default:
		didsomething = false;
		break;
	}
	if (lastMenu != MenuStack.peek()->index || lastOffset != MenuStack.peek()->offset) {
		bMenuChanged = true;
	}
	return didsomething;
}

// handle keys in run mode
bool HandleRunMode()
{
	bool didsomething = true;
	switch (ReadButton()) {
	case BTN_SELECT:
		ProcessFileOrTest();
		break;
	case BTN_RIGHT:
		if (bAllowMenuWrap || (CurrentFileIndex < NumberOfFiles - 1))
			++CurrentFileIndex;
		if (CurrentFileIndex >= NumberOfFiles)
			CurrentFileIndex = 0;
		DisplayCurrentFile();
		break;
	case BTN_LEFT:
		if (bAllowMenuWrap || (CurrentFileIndex > 0))
			--CurrentFileIndex;
		if (CurrentFileIndex < 0)
			CurrentFileIndex = NumberOfFiles - 1;
		DisplayCurrentFile();
		break;
		//case btnShowFiles:
		//	bShowBuiltInTests = !bShowBuiltInTests;
		//	GetFileNamesFromSD(currentFolder);
		//	DisplayCurrentFile();
		//	break;
	case BTN_LONG:
		OLED->clear();
		bSettingsMode = true;
		break;
	default:
		didsomething = false;
		break;
	}
	return didsomething;
}

// check buttons and return if one pressed
int ReadButton()
{
	static int nSawLongPress = 0;
	int retValue = BTN_NONE;
	// see if we got any BLE commands
	if (!sBLECommand.empty()) {
		//Serial.println(sBLECommand.c_str());
		String stmp = sBLECommand.c_str();
		stmp.toUpperCase();
		sBLECommand = "";
		if (stmp == "RUN") {
			return BTN_SELECT;
		}
		if (stmp == "RIGHT") {
			return BTN_RIGHT;
		}
		if (stmp == "LEFT") {
			return BTN_LEFT;
		}
	}
	// pull leaves retValue alone if queue is empty
	btnBuf.pull(&retValue);
	static unsigned long sawTime;
	if (nSawLongPress == 0 && retValue == BTN_LONG) {
		nSawLongPress = 1;
		//Serial.println("saw long press");
	}
	if (nSawLongPress == 1 && digitalRead(BTNPUSH)) {
		sawTime = millis();
		//Serial.println("start long timer");
		nSawLongPress = 2;
	}
	// rearm if button high again and timer over
	if (nSawLongPress == 2 && (millis() > sawTime + 100) && digitalRead(BTNPUSH)) {
		//Serial.println("rearm");
		bButtonArmed = true;
		nSawLongPress = 0;
	}
	return retValue;
}

// save or restore the display
void SaveRestoreDisplay(bool save)
{
	static uint8_t scr[1024];
	if (save) {
		memcpy(scr, OLED->buffer, sizeof(scr));
		bPauseDisplay = true;
		OLED->clear();
		OLED->display();
	}
	else {
		memcpy(OLED->buffer, scr, sizeof(scr));
		bPauseDisplay = false;
	}
}

// just check for longpress and cancel if it was there
bool CheckCancel()
{
	// if it has been set, just return true
	if (bCancelRun)
		return true;
	int button = ReadButton();
	if (button) {
		if (button == BTN_LONG) {
			bCancelRun = true;
			return true;
		}
	}
	return false;
}

//void listDir(fs::FS& fs, const char* dirname, uint8_t levels) {
//	Serial.printf("Listing directory: %s\n", dirname);
//
//	File root = fs.open(dirname);
//	if (!root) {
//		Serial.println("Failed to open directory");
//		return;
//	}
//	if (!root.isDirectory()) {
//		Serial.println("Not a directory");
//		return;
//	}
//
//	File file = root.openNextFile();
//	while (file) {
//		if (file.isDirectory()) {
//			Serial.print("  DIR : ");
//			Serial.print(file.name());
//			time_t t = file.getLastWrite();
//			struct tm* tmstruct = localtime(&t);
//			Serial.printf("  LAST WRITE: %d-%02d-%02d %02d:%02d:%02d\n", (tmstruct->tm_year) + 1900, (tmstruct->tm_mon) + 1, tmstruct->tm_mday, tmstruct->tm_hour, tmstruct->tm_min, tmstruct->tm_sec);
//			if (levels) {
//				listDir(fs, file.name(), levels - 1);
//			}
//		}
//		else {
//			Serial.print("  FILE: ");
//			Serial.print(file.name());
//			Serial.print("  SIZE: ");
//			Serial.print(file.size());
//			time_t t = file.getLastWrite();
//			struct tm* tmstruct = localtime(&t);
//			Serial.printf("  LAST WRITE: %d-%02d-%02d %02d:%02d:%02d\n", (tmstruct->tm_year) + 1900, (tmstruct->tm_mon) + 1, tmstruct->tm_mday, tmstruct->tm_hour, tmstruct->tm_min, tmstruct->tm_sec);
//		}
//		file = root.openNextFile();
//	}
//}

//void createDir(fs::FS& fs, const char* path) {
//	Serial.printf("Creating Dir: %s\n", path);
//	if (fs.mkdir(path)) {
//		Serial.println("Dir created");
//	}
//	else {
//		Serial.println("mkdir failed");
//	}
//}

//void removeDir(fs::FS& fs, const char* path) {
//	Serial.printf("Removing Dir: %s\n", path);
//	if (fs.rmdir(path)) {
//		Serial.println("Dir removed");
//	}
//	else {
//		Serial.println("rmdir failed");
//	}
//}

//void readFile(fs::FS& fs, const char* path) {
//	Serial.printf("Reading file: %s\n", path);
//
//	File file = fs.open(path);
//	if (!file) {
//		Serial.println("Failed to open file for reading");
//		return;
//	}
//
//	Serial.print("Read from file: ");
//	while (file.available()) {
//		Serial.write(file.read());
//	}
//	file.close();
//}

//void writeFile(fs::FS& fs, const char* path, const char* message) {
//	Serial.printf("Writing file: %s\n", path);
//
//	File file = fs.open(path, FILE_WRITE);
//	if (!file) {
//		Serial.println("Failed to open file for writing");
//		return;
//	}
//	if (file.print(message)) {
//		Serial.println("File written");
//	}
//	else {
//		Serial.println("Write failed");
//	}
//	file.close();
//}

//void appendFile(fs::FS& fs, const char* path, const char* message) {
//	Serial.printf("Appending to file: %s\n", path);
//
//	File file = fs.open(path, FILE_APPEND);
//	if (!file) {
//		Serial.println("Failed to open file for appending");
//		return;
//	}
//	if (file.print(message)) {
//		Serial.println("Message appended");
//	}
//	else {
//		Serial.println("Append failed");
//	}
//	file.close();
//}

//void renameFile(fs::FS& fs, const char* path1, const char* path2) {
//	Serial.printf("Renaming file %s to %s\n", path1, path2);
//	if (fs.rename(path1, path2)) {
//		Serial.println("File renamed");
//	}
//	else {
//		Serial.println("Rename failed");
//	}
//}

//void deleteFile(fs::FS& fs, const char* path) {
//	Serial.printf("Deleting file: %s\n", path);
//	if (fs.remove(path)) {
//		Serial.println("File deleted");
//	}
//	else {
//		Serial.println("Delete failed");
//	}
//}

void setupSDcard()
{
	bSdCardValid = false;
	pinMode(SDcsPin, OUTPUT);
	SPIClass(1);
	spi1.begin(18, 19, 23, SDcsPin);	// SCK,MISO,MOSI,CS

	if (!SD.begin(SDcsPin, spi1)) {
		//Serial.println("Card Mount Failed");
		return;
	}
	uint8_t cardType = SD.cardType();

	if (cardType == CARD_NONE) {
		//Serial.println("No SD card attached");
		return;
	}

	//Serial.print("SD Card Type: ");
	//if (cardType == CARD_MMC) {
	//	Serial.println("MMC");
	//}
	//else if (cardType == CARD_SD) {
	//	Serial.println("SDSC");
	//}
	//else if (cardType == CARD_SDHC) {
	//	Serial.println("SDHC");
	//}
	//else {
	//	Serial.println("UNKNOWN");
	//}

	//uint64_t cardSize = SD.cardSize() / (1024 * 1024);
	//Serial.printf("SD Card Size: %lluMB\n", cardSize);

	//listDir(SD, "/", 1);
	bSdCardValid = GetFileNamesFromSD(currentFolder);
}

// return the pixel
CRGB getRGBwithGamma() {
	if (bGammaCorrection) {
		b = gammaB[readByte(false)];
		g = gammaG[readByte(false)];
		r = gammaR[readByte(false)];
	}
	else {
		b = readByte(false);
		g = readByte(false);
		r = readByte(false);
	}
	return CRGB(r, g, b);
}

void fixRGBwithGamma(byte* rp, byte* gp, byte* bp) {
	if (bGammaCorrection) {
		*gp = gammaG[*gp];
		*bp = gammaB[*bp];
		*rp = gammaR[*rp];
	}
}

// up to 32 bouncing balls
void TestBouncingBalls() {
	CRGB colors[] = {
		CRGB::Red,
		CRGB::White,
		CRGB::Blue,
		CRGB::Green,
		CRGB::Yellow,
		CRGB::Cyan,
		CRGB::Magenta,
		CRGB::Grey,
		CRGB::RosyBrown,
		CRGB::RoyalBlue,
		CRGB::SaddleBrown,
		CRGB::Salmon,
		CRGB::SandyBrown,
		CRGB::SeaGreen,
		CRGB::Seashell,
		CRGB::Sienna,
		CRGB::Silver,
		CRGB::SkyBlue,
		CRGB::SlateBlue,
		CRGB::SlateGray,
		CRGB::SlateGrey,
		CRGB::Snow,
		CRGB::SpringGreen,
		CRGB::SteelBlue,
		CRGB::Tan,
		CRGB::Teal,
		CRGB::Thistle,
		CRGB::Tomato,
		CRGB::Turquoise,
		CRGB::Violet,
		CRGB::Wheat,
		CRGB::WhiteSmoke,
	};

	BouncingColoredBalls(nBouncingBallsCount, colors);
	FastLED.clear(true);
}

void BouncingColoredBalls(int balls, CRGB colors[]) {
	time_t startsec = time(NULL);
	float Gravity = -9.81;
	int StartHeight = 1;

	float* Height = (float*)calloc(balls, sizeof(float));
	float* ImpactVelocity = (float*)calloc(balls, sizeof(float));
	float* TimeSinceLastBounce = (float*)calloc(balls, sizeof(float));
	int* Position = (int*)calloc(balls, sizeof(int));
	long* ClockTimeSinceLastBounce = (long*)calloc(balls, sizeof(long));
	float* Dampening = (float*)calloc(balls, sizeof(float));
	float ImpactVelocityStart = sqrt(-2 * Gravity * StartHeight);

	for (int i = 0; i < balls; i++) {
		ClockTimeSinceLastBounce[i] = millis();
		Height[i] = StartHeight;
		Position[i] = 0;
		ImpactVelocity[i] = ImpactVelocityStart;
		TimeSinceLastBounce[i] = 0;
		Dampening[i] = 0.90 - float(i) / pow(balls, 2);
	}

	// run for allowed seconds
	long start = millis();
	long percent;
	//nTimerSeconds = nBouncingBallsRuntime;
	//int lastSeconds = 0;
	//EventTimers.every(1000L, SecondsTimer);
	while (millis() < start + ((long)nBouncingBallsRuntime * 1000)) {
		ShowProgressBar((time(NULL) - startsec) * 100 / nBouncingBallsRuntime);
		//if (nTimerSeconds != lastSeconds) {
		//    tft.setTextColor(ILI9341_WHITE, ILI9341_BLACK);
		//    tft.setTextSize(2);
		//    tft.setCursor(0, 38);
		//    char num[50];
		//    nTimerSeconds = nTimerSeconds;
		//    sprintf(num, "%3d seconds", nTimerSeconds);
		//    tft.print(num);
		//}
		//percent = map(millis() - start, 0, nBouncingBallsRuntime * 1000, 0, 100);
		//ShowProgressBar(percent);
		if (CheckCancel())
			return;
		for (int i = 0; i < balls; i++) {
			if (CheckCancel())
				return;
			TimeSinceLastBounce[i] = millis() - ClockTimeSinceLastBounce[i];
			Height[i] = 0.5 * Gravity * pow(TimeSinceLastBounce[i] / nBouncingBallsDecay, 2.0) + ImpactVelocity[i] * TimeSinceLastBounce[i] / nBouncingBallsDecay;

			if (Height[i] < 0) {
				Height[i] = 0;
				ImpactVelocity[i] = Dampening[i] * ImpactVelocity[i];
				ClockTimeSinceLastBounce[i] = millis();

				if (ImpactVelocity[i] < 0.01) {
					ImpactVelocity[i] = ImpactVelocityStart;
				}
			}
			Position[i] = round(Height[i] * (STRIPLENGTH - 1) / StartHeight);
		}

		for (int i = 0; i < balls; i++) {
			if (CheckCancel())
				return;
			SetPixel(Position[i], colors[i]);
		}
		FastLED.show();
		delayMicroseconds(50);
		FastLED.clear();
	}
	free(Height);
	free(ImpactVelocity);
	free(TimeSinceLastBounce);
	free(Position);
	free(ClockTimeSinceLastBounce);
	free(Dampening);
}

#define BARBERSIZE 10
#define BARBERCOUNT 40
void BarberPole()
{
CRGB:CRGB red, white, blue;
	byte r, g, b;
	r = 255, g = 0, b = 0;
	fixRGBwithGamma(&r, &g, &b);
	red = CRGB(r, g, b);
	r = 255, g = 255, b = 255;
	fixRGBwithGamma(&r, &g, &b);
	white = CRGB(r, g, b);
	r = 0, g = 0, b = 255;
	fixRGBwithGamma(&r, &g, &b);
	blue = CRGB(r, g, b);
	//ShowProgressBar(0);
	for (int loop = 0; loop < (8 * BARBERCOUNT); ++loop) {
		//Serial.println("barber:" + String(loop));
		if ((loop % 10) == 0)
			ShowProgressBar(loop * 100 / (8 * BARBERCOUNT));
		if (CheckCancel())
			return;
		for (int ledIx = 0; ledIx < STRIPLENGTH; ++ledIx) {
			if (CheckCancel())
				return;
			// figure out what color
			switch (((ledIx + loop) % BARBERCOUNT) / BARBERSIZE) {
			case 0: // red
				SetPixel(ledIx, red);
				break;
			case 1: // white
			case 3:
				SetPixel(ledIx, white);
				break;
			case 2: // blue
				SetPixel(ledIx, blue);
				break;
			}
		}
		FastLED.show();
		delay(frameHold);
	}
	ShowProgressBar(100);
}

// checkerboard
void CheckerBoard()
{
	time_t start = time(NULL);
	int width = nCheckboardBlackWidth + nCheckboardWhiteWidth;
	bStripWaiting = true;
	int times = 0;
	CRGB color1 = CRGB::Black, color2 = CRGB::White;
	esp_timer_start_once(oneshot_LED_timer, nCheckerBoardRuntime * 1000000);
	int addPixels = 0;
	while (bStripWaiting) {
		for (int y = 0; y < STRIPLENGTH; ++y) {
			SetPixel(y, ((y + addPixels) % width) < nCheckboardBlackWidth ? color1 : color2);
		}
		FastLED.show();
		ShowProgressBar((time(NULL) - start) * 100 /nCheckerBoardRuntime);
		int count = nCheckerboardHoldframes;
		while (count-- > 0) {
			delay(frameHold);
			if (CheckCancel()) {
				esp_timer_stop(oneshot_LED_timer);
				bStripWaiting = false;
				break;
			}
		}
		if (bCheckerBoardAlternate && (times++ % 2)) {
			// swap colors
			CRGB temp = color1;
			color1 = color2;
			color2 = temp;
		}
		addPixels += nCheckerboardAddPixels;
	}
}

void RandomBars()
{
	ShowRandomBars(bRandomBarsBlacks, nRandomBarsRuntime);
}

// show random bars of lights with optional blacks between
void ShowRandomBars(bool blacks, int runtime)
{
	time_t start = time(NULL);
	byte r, g, b;
	srand(millis());
	char line[40];
	bStripWaiting = true;
	esp_timer_start_once(oneshot_LED_timer, runtime * 1000000);
	for (int pass = 0; bStripWaiting; ++pass) {
		ShowProgressBar((time(NULL) - start) * 100 / runtime);
		if (blacks && (pass % 2)) {
			// odd numbers, clear
			FastLED.clear(true);
		}
		else {
			// even numbers, show bar
			r = random(0, 255);
			g = random(0, 255);
			b = random(0, 255);
			fixRGBwithGamma(&r, &g, &b);
			// fill the strip color
			FastLED.showColor(CRGB(r, g, b));
		}
		int count = nRandomBarsHoldframes;
		while (count-- > 0) {
			delay(frameHold);
			if (CheckCancel()) {
				esp_timer_stop(oneshot_LED_timer);
				bStripWaiting = false;
				break;
			}
		}
	}
}

// running dot
void RunningDot()
{
	for (int colorvalue = 0; colorvalue <= 3; ++colorvalue) {
		if (CheckCancel())
			return;
		// RGBW
		byte r, g, b;
		switch (colorvalue) {
		case 0: // red
			r = 255;
			g = 0;
			b = 0;
			break;
		case 1: // green
			r = 0;
			g = 255;
			b = 0;
			break;
		case 2: // blue
			r = 0;
			g = 0;
			b = 255;
			break;
		case 3: // white
			r = 255;
			g = 255;
			b = 255;
			break;
		}
		fixRGBwithGamma(&r, &g, &b);
		char line[10];
		for (int ix = 0; ix < STRIPLENGTH; ++ix) {
			if (CheckCancel())
				return;
			//lcd.setCursor(11, 0);
			//sprintf(line, "%3d", ix);
			//lcd.print(line);
			if (ix > 0) {
				SetPixel(ix - 1, CRGB::Black);
			}
			SetPixel(ix, CRGB(r, g, b));
			FastLED.show();
			delay(frameHold);
		}
		// remember the last one, turn it off
		SetPixel(STRIPLENGTH - 1, CRGB::Black);
		FastLED.show();
	}
}

void OppositeRunningDots()
{
	for (int mode = 0; mode <= 3; ++mode) {
		if (CheckCancel())
			return;
		// RGBW
		byte r, g, b;
		switch (mode) {
		case 0: // red
			r = 255;
			g = 0;
			b = 0;
			break;
		case 1: // green
			r = 0;
			g = 255;
			b = 0;
			break;
		case 2: // blue
			r = 0;
			g = 0;
			b = 255;
			break;
		case 3: // white
			r = 255;
			g = 255;
			b = 255;
			break;
		}
		fixRGBwithGamma(&r, &g, &b);
		for (int ix = 0; ix < STRIPLENGTH; ++ix) {
			if (CheckCancel())
				return;
			if (ix > 0) {
				SetPixel(ix - 1, CRGB::Black);
				SetPixel(STRIPLENGTH - ix + 1, CRGB::Black);
			}
			SetPixel(STRIPLENGTH - ix, CRGB(r, g, b));
			SetPixel(ix, CRGB(r, g, b));
			FastLED.show();
			delay(frameHold);
		}
	}
}

// show all in a color
void DisplayAllColor()
{
	if (bDisplayAllRGB)
		FastLED.showColor(CRGB(nDisplayAllRed, nDisplayAllGreen, nDisplayAllBlue));
	else
		FastLED.showColor(CHSV(nDisplayAllHue, nDisplayAllSaturation, nDisplayAllBrightness));
	// show until cancelled, but check for rotations of the knob
	int btn;
	int what = 0;	// 0 for hue, 1 for saturation, 2 for brightness, 3 for increment
	int increment = 10;
	bool bChange = true;
	while (true) {
		if (bChange) {
			switch (what) {
			case 0:
				if (bDisplayAllRGB)
					DisplayLine(2, "Change Red: " + String(nDisplayAllRed));
				else
					DisplayLine(2, "Change HUE: " + String(nDisplayAllHue));
				break;
			case 1:
				if (bDisplayAllRGB)
					DisplayLine(2, "Change Green: " + String(nDisplayAllGreen));
				else 
					DisplayLine(2, "Change Saturation: " + String(nDisplayAllSaturation));
				break;
			case 2:
				if (bDisplayAllRGB)
					DisplayLine(2, "Change Blue: " + String(nDisplayAllBlue));
				else 
					DisplayLine(2, "Change Brightness: " + String(nDisplayAllBrightness));
				break;
			case 3:
				DisplayLine(2, "Increment: " + String(increment));
				break;
			}
		}
		btn = ReadButton();
		bChange = true;
		switch (btn) {
		case BTN_NONE:
			bChange = false;
			break;
		case BTN_RIGHT:
			switch (what) {
			case 0:
				if (bDisplayAllRGB)
					nDisplayAllRed += increment;
				else 
					nDisplayAllHue += increment;
				break;
			case 1:
				if (bDisplayAllRGB)
					nDisplayAllGreen += increment;
				else 
					nDisplayAllSaturation += increment;
				break;
			case 2:
				if (bDisplayAllRGB)
					nDisplayAllBlue += increment;
				else
					nDisplayAllBrightness += increment;
				break;
			case 3:
				increment *= 10;
				break;
			}
			break;
		case BTN_LEFT:
			switch (what) {
			case 0:
				if (bDisplayAllRGB)
					nDisplayAllRed -= increment;
				else 
					nDisplayAllHue -= increment;
				break;
			case 1:
				if (bDisplayAllRGB)
					nDisplayAllGreen -= increment;
				else
					nDisplayAllSaturation -= increment;
				break;
			case 2:
				if (bDisplayAllRGB)
					nDisplayAllBlue -= increment;
				else
					nDisplayAllBrightness -= increment;
				break;
			case 3:
				increment /= 10;
				break;
			}
			break;
		case BTN_SELECT:
			what = ++what % 4;
			break;
		case BTN_LONG:
			// put it back, we don't want it
			btnBuf.add(btn);
			break;
		}
		if (CheckCancel())
			return;
		if (bChange) {
			increment = constrain(increment, 1, 100);
			if (bDisplayAllRGB) {
				nDisplayAllRed = constrain(nDisplayAllRed, 0, 255);
				nDisplayAllGreen = constrain(nDisplayAllGreen, 0, 255);
				nDisplayAllBlue = constrain(nDisplayAllBlue, 0, 255);
				FastLED.showColor(CRGB(nDisplayAllRed, nDisplayAllGreen, nDisplayAllBlue));
			}
			else {
				nDisplayAllHue = constrain(nDisplayAllHue, 0, 255);
				nDisplayAllSaturation = constrain(nDisplayAllSaturation, 0, 255);
				nDisplayAllBrightness = constrain(nDisplayAllBrightness, 0, 255);
				FastLED.showColor(CHSV(nDisplayAllHue, nDisplayAllSaturation, nDisplayAllBrightness));
			}
		}
		delay(10);
	}
}

void TestTwinkle() {
	TwinkleRandom(nTwinkleRuntime, frameHold, bTwinkleOnlyOne);
}
void TwinkleRandom(int Runtime, int SpeedDelay, boolean OnlyOne) {
	time_t start = time(NULL);
	bStripWaiting = true;
	esp_timer_start_once(oneshot_LED_timer, Runtime * 1000000);
	while (bStripWaiting) {
		ShowProgressBar((time(NULL) - start) * 100 / nTwinkleRuntime);
		SetPixel(random(STRIPLENGTH), CRGB(random(0, 255), random(0, 255), random(0, 255)));
		FastLED.show();
		delay(SpeedDelay);
		if (OnlyOne) {
			FastLED.clear(true);
		}
		if (CheckCancel()) {
			esp_timer_stop(oneshot_LED_timer);
			bStripWaiting = false;
		}
	}
}

void TestCylon()
{
	CylonBounce(nCylonEyeRed, nCylonEyeGreen, nCylonEyeBlue, nCylonEyeSize, frameHold, 50);
}
void CylonBounce(byte red, byte green, byte blue, int EyeSize, int SpeedDelay, int ReturnDelay)
{
	for (int i = 0; i < STRIPLENGTH - EyeSize - 2; i++) {
		if (CheckCancel())
			return;
		FastLED.clear();
		SetPixel(i, CRGB(red / 10, green / 10, blue / 10));
		for (int j = 1; j <= EyeSize; j++) {
			SetPixel(i + j, CRGB(red, green, blue));
		}
		SetPixel(i + EyeSize + 1, CRGB(red / 10, green / 10, blue / 10));
		FastLED.show();
		delay(SpeedDelay);
	}
	delay(ReturnDelay);

	for (int i = STRIPLENGTH - EyeSize - 2; i > 0; i--) {
		if (CheckCancel())
			return;
		FastLED.clear();
		SetPixel(i, CRGB(red / 10, green / 10, blue / 10));
		for (int j = 1; j <= EyeSize; j++) {
			SetPixel(i + j, CRGB(red, green, blue));
		}
		SetPixel(i + EyeSize + 1, CRGB(red / 10, green / 10, blue / 10));
		FastLED.show();
		delay(SpeedDelay);
	}
	delay(ReturnDelay);
}

void TestMeteor() {
	meteorRain(nMeteorRed, nMeteorGreen, nMeteorBlue, nMeteorSize, 64, true, 30);
}

void meteorRain(byte red, byte green, byte blue, byte meteorSize, byte meteorTrailDecay, boolean meteorRandomDecay, int SpeedDelay) {
	FastLED.clear(true);

	for (int i = 0; i < STRIPLENGTH + STRIPLENGTH; i++) {
		if (CheckCancel())
			return;
		// fade brightness all LEDs one step
		for (int j = 0; j < STRIPLENGTH; j++) {
			if (CheckCancel())
				return;
			if ((!meteorRandomDecay) || (random(10) > 5)) {
				fadeToBlack(j, meteorTrailDecay);
			}
		}

		// draw meteor
		for (int j = 0; j < meteorSize; j++) {
			if (CheckCancel())
				return;
			if ((i - j < STRIPLENGTH) && (i - j >= 0)) {
				SetPixel(i - j, CRGB(red, green, blue));
			}
		}
		FastLED.show();
		delay(SpeedDelay);
	}
}

void TestConfetti()
{
	time_t start = time(NULL);
	gHue = 0;
	bStripWaiting = true;
	esp_timer_start_once(oneshot_LED_timer, nConfettiRuntime * 1000000);
	while (bStripWaiting) {
		EVERY_N_MILLISECONDS(frameHold) {
			if (bConfettiCycleHue)
				++gHue;
			confetti();
			FastLED.show();
			ShowProgressBar((time(NULL) - start) * 100 / nConfettiRuntime);
		}
		if (CheckCancel()) {
			esp_timer_stop(oneshot_LED_timer);
			bStripWaiting = false;
		}
	}
	// wait for timeout so strip will be blank
	delay(100);
}

void confetti()
{
	// random colored speckles that blink in and fade smoothly
	fadeToBlackBy(leds, STRIPLENGTH, 10);
	int pos = random16(STRIPLENGTH);
	leds[pos] += CHSV(gHue + random8(64), 200, 255);
}

void TestJuggle()
{
	time_t start = time(NULL);
	bStripWaiting = true;
	esp_timer_start_once(oneshot_LED_timer, nJuggleRuntime * 1000000);
	while (bStripWaiting) {
		EVERY_N_MILLISECONDS(frameHold) {
			juggle();
			FastLED.show();
			ShowProgressBar((time(NULL) - start) * 100 / nJuggleRuntime);
		}
		if (CheckCancel()) {
			esp_timer_stop(oneshot_LED_timer);
			bStripWaiting = false;
		}
	}
}

void juggle()
{
	// eight colored dots, weaving in and out of sync with each other
	fadeToBlackBy(leds, STRIPLENGTH, 20);
	byte dothue = 0;
	for (int i = 0; i < 8; i++) {
		leds[beatsin16(i + 7, 0, STRIPLENGTH)] |= CHSV(dothue, 200, 255);
		dothue += 32;
	}
}

void TestSine()
{
	time_t start = time(NULL);
	gHue = nSineStartingHue;
	bStripWaiting = true;
	esp_timer_start_once(oneshot_LED_timer, nSineRuntime * 1000000);
	while (bStripWaiting) {
		EVERY_N_MILLISECONDS(frameHold) {
			sinelon();
			FastLED.show();
			ShowProgressBar((time(NULL) - start) * 100 / nSineRuntime);
		}
		if (CheckCancel()) {
			esp_timer_stop(oneshot_LED_timer);
			bStripWaiting = false;
		}
	}
}
void sinelon()
{
	// a colored dot sweeping back and forth, with fading trails
	fadeToBlackBy(leds, STRIPLENGTH, 20);
	int pos = beatsin16(nSineSpeed, 0, STRIPLENGTH);
	leds[pos] += CHSV(gHue, 255, 192);
	if (bSineCycleHue)
		++gHue;
}

void TestBpm()
{
	time_t start = time(NULL);
	gHue = 0;
	bStripWaiting = true;
	esp_timer_start_once(oneshot_LED_timer, nBpmRuntime * 1000000);
	while (bStripWaiting) {
		EVERY_N_MILLISECONDS(frameHold) {
			bpm();
			FastLED.show();
			ShowProgressBar((time(NULL) - start) * 100 / nBpmRuntime);
		}
		if (CheckCancel()) {
			esp_timer_stop(oneshot_LED_timer);
			bStripWaiting = false;
		}
	}
}

void bpm()
{
	// colored stripes pulsing at a defined Beats-Per-Minute (BPM)
	CRGBPalette16 palette = PartyColors_p;
	uint8_t beat = beatsin8(nBpmBeatsPerMinute, 64, 255);
	for (int i = 0; i < STRIPLENGTH; i++) { //9948
		SetPixel(i, ColorFromPalette(palette, gHue + (i * 2), beat - gHue + (i * 10)));
	}
	if (bBpmCycleHue)
		++gHue;
}

void TestRainbow()
{
	time_t start = time(NULL);
	gHue = 0;
	bStripWaiting = true;
	esp_timer_start_once(oneshot_LED_timer, nRainbowRuntime * 1000000);
	fill_rainbow(leds, STRIPLENGTH, gHue, nRainbowRepeats);
	FadeInOut(nRainbowFadeTime * 100, true);
	while (bStripWaiting) {
		EVERY_N_MILLISECONDS(frameHold) {
			if (bRainbowCycleHue)
				++gHue;
			fill_rainbow(leds, STRIPLENGTH, gHue, nRainbowRepeats);
			if (bRainbowAddGlitter)
				addGlitter(80);
			FastLED.show();
			ShowProgressBar((time(NULL) - start) * 100 / nRainbowRuntime);
		}
		if (CheckCancel()) {
			esp_timer_stop(oneshot_LED_timer);
			return;
		}
	}
	FadeInOut(nRainbowFadeTime * 100, false);
}

// time is in mSec
void FadeInOut(int time, bool in)
{
	if (in) {
		for (int i = 0; i <= nStripBrightness; ++i) {
			FastLED.setBrightness(i);
			FastLED.show();
			delay(time / nStripBrightness);
		}
	}
	else {
		for (int i = nStripBrightness; i >= 0; --i) {
			FastLED.setBrightness(i);
			FastLED.show();
			delay(time / nStripBrightness);
		}
	}
}

void addGlitter(fract8 chanceOfGlitter)
{
	if (random8() < chanceOfGlitter) {
		leds[random16(STRIPLENGTH)] += CRGB::White;
	}
}

void fadeToBlack(int ledNo, byte fadeValue) {
	// FastLED
	leds[ledNo].fadeToBlackBy(fadeValue);
}

// run file or built-in
void ProcessFileOrTest()
{
	String line;
	// let's see if this is a folder command
	String tmp = FileNames[CurrentFileIndex];
	if (tmp[0] == NEXT_FOLDER_CHAR) {
		FileIndexStack.push(CurrentFileIndex);
		tmp = tmp.substring(1);
		// change folder, reload files
		currentFolder += tmp + "/";
		GetFileNamesFromSD(currentFolder);
		DisplayCurrentFile();
		return;
	}
	else if (tmp[0] == PREVIOUS_FOLDER_CHAR) {
		tmp = tmp.substring(1);
		tmp = tmp.substring(0, tmp.length() - 1);
		if (tmp.length() == 0)
			tmp = "/";
		// change folder, reload files
		currentFolder = tmp;
		GetFileNamesFromSD(currentFolder);
		CurrentFileIndex = FileIndexStack.pop();
		DisplayCurrentFile();
		return;
	}
	bIsRunning = true;
	nProgress = 0;
	DisplayLine(3, "");
	//DisplayCurrentFile();
	if (startDelay) {
		// set a timer
		nTimerSeconds = startDelay;
		while (nTimerSeconds) {
			//Serial.println("timer " + String(nTimerSeconds));
			line = "Start Delay: " + String(nTimerSeconds / 10) + "." + String(nTimerSeconds % 10);
			DisplayLine(1, line);
			if (CheckCancel())
				break;
			delay(100);
			--nTimerSeconds;
		}
		DisplayLine(2, "");
	}
	int chainCount = bChainFiles ? FileCountOnly() - CurrentFileIndex : 1;
	int chainRepeatCount = bChainFiles ? nChainRepeats : 1;
	int lastFileIndex = CurrentFileIndex;
	if (bShowBuiltInTests) {
		chainCount = 1;
		chainRepeatCount = 1;
	}
	// set the basic LED info
	FastLED.setTemperature(CRGB(whiteBalance.r, whiteBalance.g, whiteBalance.b));
	FastLED.setBrightness(nStripBrightness);
	line = "";
	while (chainRepeatCount-- > 0) {
		while (chainCount-- > 0) {
			DisplayCurrentFile();
			if (bChainFiles && !bShowBuiltInTests) {
				line = "Files: " + String(chainCount + 1);
				DisplayLine(3, line);
				line = "";
			}
			// process the repeats and waits for each file in the list
			for (nRepeatsLeft = repeatCount; nRepeatsLeft > 0; nRepeatsLeft--) {
				// fill the progress bar
				if (!bShowBuiltInTests)
					ShowProgressBar(0);
				if (repeatCount > 1) {
					line = "Repeats: " + String(nRepeatsLeft)+" ";
				}
				if (nChainRepeats > 1) {
					line += "Chains: " + String(chainRepeatCount + 1);
				}
				DisplayLine(2, line);
				if (bShowBuiltInTests) {
					DisplayLine(3, "Running (long cancel)");
					// run the test
					(*BuiltInFiles[CurrentFileIndex].function)();
				}
				else {
					// output the file
					SendFile(FileNames[CurrentFileIndex]);
				}
				if (bCancelRun) {
					break;
				}
				if (!bShowBuiltInTests)
					ShowProgressBar(0);
				if (nRepeatsLeft > 1) {
					if (repeatDelay) {
						FastLED.clear(true);
						// start timer
						nTimerSeconds = repeatDelay;
						while (nTimerSeconds > 0) {
							line = "Repeat Delay: " + String(nTimerSeconds / 10) + "." + String(nTimerSeconds % 10);
							DisplayLine(1, line);
							line = "";
							if (CheckCancel())
								break;
							delay(100);
							--nTimerSeconds;
						}
						//DisplayLine(2, "");
					}
				}
			}
			if (bCancelRun) {
				chainCount = 0;
				break;
			}
			if (bShowBuiltInTests)
				break;
			// see if we are chaining, if so, get the next file, if a folder we're done
			if (bChainFiles) {
				// grab the next file
				if (CurrentFileIndex < NumberOfFiles - 1)
					++CurrentFileIndex;
				if (IsFolder(CurrentFileIndex))
					break;
			}
			line = "";
			// clear
			FastLED.clear(true);
		}
		if (bCancelRun) {
			chainCount = 0;
			chainRepeatCount = 0;
			bCancelRun = false;
			break;
		}
		// start again
		CurrentFileIndex = lastFileIndex;
		chainCount = bChainFiles ? FileCountOnly() - CurrentFileIndex : 1;
	}
	if (bChainFiles)
		CurrentFileIndex = lastFileIndex;
	FastLED.clear(true);
	OLED->clear();
	bIsRunning = false;
	DisplayCurrentFile();
	nProgress = 0;
	// clear buttons
	int btn;
	while (btnBuf.pull(&btn))
		;
}

void SendFile(String Filename) {
	char temp[14];
	Filename.toCharArray(temp, 14);
	// see if there is an associated config file
	String cfFile = temp;
	cfFile = MakeIPCFilename(cfFile, true);
	SettingsSaveRestore(true);
	ProcessConfigFile(cfFile);
	String fn = currentFolder + temp;
	dataFile = SD.open(fn);
	// if the file is available send it to the LED's
	if (dataFile.available()) {
		for (int cnt = 0; cnt < (bMirrorPlayImage ? 2 : 1); ++cnt) {
			ReadAndDisplayFile(cnt == 0);
			bReverseImage = !bReverseImage; // note this will be restored by SettingsSaveRestore
			dataFile.seek(0);
			if (CheckCancel())
				break;
		}
		dataFile.close();
	}
	else {
		//lcd.clear();
		//lcd.print("* Error reading ");
		//lcd.setCursor(0, 1);
		//lcd.print(CurrentFilename);
		//bBackLightOn = true;
		//delay(1000);
		//lcd.clear();
		//setupSDcard();
		return;
	}
	ShowProgressBar(100);
	SettingsSaveRestore(false);
}

void IRAM_ATTR ReadAndDisplayFile(bool doingFirstHalf) {
	static int totalSeconds;
	if (doingFirstHalf)
		totalSeconds = -1;
#define MYBMP_BF_TYPE           0x4D42
#define MYBMP_BF_OFF_BITS       54
#define MYBMP_BI_SIZE           40
#define MYBMP_BI_RGB            0L
#define MYBMP_BI_RLE8           1L
#define MYBMP_BI_RLE4           2L
#define MYBMP_BI_BITFIELDS      3L

	uint16_t bmpType = readInt();
	uint32_t bmpSize = readLong();
	uint16_t bmpReserved1 = readInt();
	uint16_t bmpReserved2 = readInt();
	uint32_t bmpOffBits = readLong();
	bmpOffBits = 54;

	/* Check file header */
	if (bmpType != MYBMP_BF_TYPE || bmpOffBits != MYBMP_BF_OFF_BITS) {
		WriteMessage(String("Invalid BMP:\n") + currentFolder + FileNames[CurrentFileIndex], true);
		return;
	}

	/* Read info header */
	uint32_t imgSize = readLong();
	uint32_t imgWidth = readLong();
	uint32_t imgHeight = readLong();
	uint16_t imgPlanes = readInt();
	uint16_t imgBitCount = readInt();
	uint32_t imgCompression = readLong();
	uint32_t imgSizeImage = readLong();
	uint32_t imgXPelsPerMeter = readLong();
	uint32_t imgYPelsPerMeter = readLong();
	uint32_t imgClrUsed = readLong();
	uint32_t imgClrImportant = readLong();

	/* Check info header */
	if (imgSize != MYBMP_BI_SIZE || imgWidth <= 0 ||
		imgHeight <= 0 || imgPlanes != 1 ||
		imgBitCount != 24 || imgCompression != MYBMP_BI_RGB ||
		imgSizeImage == 0)
	{
		WriteMessage(String("Unsupported, must be 24bpp:\n") + currentFolder + FileNames[CurrentFileIndex], true);
		return;
	}

	int displayWidth = imgWidth;
	if (imgWidth > STRIPLENGTH) {
		displayWidth = STRIPLENGTH;           //only display the number of led's we have
	}

	/* compute the line length */
	uint32_t lineLength = imgWidth * 3;
	// fix for padding to 4 byte words
	if ((lineLength % 4) != 0)
		lineLength = (lineLength / 4 + 1) * 4;

	// Note:  
	// The x,r,b,g sequence below might need to be changed if your strip is displaying
	// incorrect colors.  Some strips use an x,r,b,g sequence and some use x,r,g,b
	// Change the order if needed to make the colors correct.

	long secondsLeft = 0, lastSeconds = 0;
	char num[50];
	int percent;
	unsigned minLoopTime = 0; // the minimum time it takes to process a line
	bool bLoopTimed = false;
	// note that y is 0 based and x is 0 based in the following code, the original code had y 1 based
	for (int y = bReverseImage ? 0 : imgHeight - 1; bReverseImage ? y < imgHeight : y >= 0; bReverseImage ? ++y : --y) {
		// approximate time left
		if (bReverseImage)
			secondsLeft = ((long)(imgHeight - y) * (frameHold + minLoopTime) / 1000L) + 1;
		else
			secondsLeft = ((long)y * (frameHold + minLoopTime) / 1000L) + 1;
		// mark the time for timing the loop
		if (!bLoopTimed)
			minLoopTime = millis();
		if (bMirrorPlayImage) {
			if (totalSeconds == -1)
				totalSeconds = secondsLeft;
			if (doingFirstHalf) {
				secondsLeft += totalSeconds;
			}
		}
		if (secondsLeft != lastSeconds) {
			lastSeconds = secondsLeft;
			sprintf(num, "File Seconds: %d", secondsLeft);
			DisplayLine(1, num);
		}
		percent = map(bReverseImage ? y : imgHeight - y, 0, imgHeight, 0, 100);
		if (bMirrorPlayImage) {
			percent /= 2;
			if (!doingFirstHalf) {
				percent += 50;
			}
		}
		if (((percent % 5) == 0) || percent > 90) {
			ShowProgressBar(percent);
		}
		int bufpos = 0;
		//uint32_t offset = (MYBMP_BF_OFF_BITS + (y * lineLength));
		//dataFile.seekSet(offset);
		CRGB pixel;
		// get to start of pixel data, moved this out of the loop below to speed things up
		FileSeek((uint32_t)MYBMP_BF_OFF_BITS + (y * lineLength));
		for (int x = 0; x < displayWidth; x++) {
			//FileSeek((uint32_t)MYBMP_BF_OFF_BITS + ((y * lineLength) + (x * 3)));
			//dataFile.seekSet((uint32_t)MYBMP_BF_OFF_BITS + ((y * lineLength) + (x * 3)));
			// this reads three bytes
			pixel = getRGBwithGamma();
			// see if we want this one
			if (bScaleHeight && (x * displayWidth) % imgWidth) {
				continue;
			}
			SetPixel(x, pixel);
		}
		// see how long it took to get here
		if (!bLoopTimed) {
			minLoopTime = millis() - minLoopTime;
			bLoopTimed = true;
		}
		//Serial.println("loop: " + String(minLoopTime));
		// wait for timer to expire before we show the next frame
		while (bStripWaiting) {
			delayMicroseconds(100);
			// we should maybe check the cancel key here to handle slow frame rates?
		}
		// now show the lights
		FastLED.show();
		// set a timer while we go ahead and load the next frame
		bStripWaiting = true;
		esp_timer_start_once(oneshot_LED_timer, frameHold * 1000);
		// check keys
		if (CheckCancel())
			break;
		// check if frame advance button requested
		if (nFramePulseCount) {
			for (int ix = nFramePulseCount; ix; --ix) {
				// wait for press
				while (digitalRead(FRAMEBUTTON)) {
					if (CheckCancel())
						break;
					delay(10);
				}
				// wait for release
				while (!digitalRead(FRAMEBUTTON)) {
					if (CheckCancel())
						break;
					delay(10);
				}
			}
		}
		if (bManualFrameAdvance) {
			int btn;
			bool done = false;
			while (!done) {
				btn = ReadButton();
				if (btn == BTN_NONE)
					continue;
				else if (btn == BTN_LONG)
					btnBuf.add(btn);
				else
					break;
				if (CheckCancel())
					break;
				delay(10);
			}
		}
		if (bCancelRun)
			break;
	}
	// all done
	readByte(true);
}

void DisplayLine(int line, String text, bool bOverRide)
{
	if (bPauseDisplay && !bOverRide)
		return;
	int y = line * charHeight + (bSettingsMode ? 0 : 6);
	//int wide = OLED->getStringWidth(num);
	OLED->setColor(BLACK);
	OLED->fillRect(0, y, /*wide*/OLED->getWidth(), charHeight);
	OLED->setColor(WHITE);
	OLED->drawString(0, y, text);
	OLED->display();
}

// the star is used to indicate active menu line
void DisplayMenuLine(int line, int displine, String text)
{
	String mline = (MenuStack.peek()->index == line ? "* " : "  ") + text;
	//if (MenuStack.peek()->index == line)
		//OLED->setFont(SansSerif_bold_10);
	DisplayLine(displine, mline);
	//OLED->setFont(ArialMT_Plain_10);
}

uint32_t readLong() {
	uint32_t retValue;
	byte incomingbyte;

	incomingbyte = readByte(false);
	retValue = (uint32_t)((byte)incomingbyte);

	incomingbyte = readByte(false);
	retValue += (uint32_t)((byte)incomingbyte) << 8;

	incomingbyte = readByte(false);
	retValue += (uint32_t)((byte)incomingbyte) << 16;

	incomingbyte = readByte(false);
	retValue += (uint32_t)((byte)incomingbyte) << 24;

	return retValue;
}

uint16_t readInt() {
	byte incomingbyte;
	uint16_t retValue;

	incomingbyte = readByte(false);
	retValue += (uint16_t)((byte)incomingbyte);

	incomingbyte = readByte(false);
	retValue += (uint16_t)((byte)incomingbyte) << 8;

	return retValue;
}
byte filebuf[512];
int fileindex = 0;
int filebufsize = 0;
uint32_t filePosition = 0;

int readByte(bool clear) {
	//int retbyte = -1;
	if (clear) {
		filebufsize = 0;
		fileindex = 0;
		return 0;
	}
	// TODO: this needs to align with 512 byte boundaries, maybe
	if (filebufsize == 0 || fileindex >= sizeof(filebuf)) {
		filePosition = dataFile.position();
		//// if not on 512 boundary yet, just return a byte
		//if ((filePosition % 512) && filebufsize == 0) {
		//    //Serial.println("not on 512");
		//    return dataFile.read();
		//}
		// read a block
//        Serial.println("block read");
		do {
			filebufsize = dataFile.read(filebuf, sizeof(filebuf));
		} while (filebufsize < 0);
		fileindex = 0;
	}
	return filebuf[fileindex++];
	//while (retbyte < 0) 
	//    retbyte = dataFile.read();
	//return retbyte;
}

// make sure we are the right place
uint32_t FileSeek(uint32_t place)
{
	if (place < filePosition || place >= filePosition + filebufsize) {
		// we need to read some more
		filebufsize = 0;
		dataFile.seek(place);
	}
}

// count the actual files
int FileCountOnly()
{
	int count = 0;
	// ignore folders, at the end
	for (int files = 0; files < NumberOfFiles; ++files) {
		if (!IsFolder(count))
			++count;
	}
	return count;
}

// return true if current file is folder
bool IsFolder(int index)
{
	return FileNames[index][0] == NEXT_FOLDER_CHAR
		|| FileNames[index][0] == PREVIOUS_FOLDER_CHAR;
}

// show the current file
void DisplayCurrentFile(bool path)
{
	//String name = FileNames[CurrentFileIndex];
	//String upper = name;
	//upper.toUpperCase();
 //   if (upper.endsWith(".BMP"))
 //       name = name.substring(0, name.length() - 4);
	if (bShowBuiltInTests) {
		DisplayLine(0, FileNames[CurrentFileIndex]);
	}
	else {
		if (bSdCardValid) {
			DisplayLine(0, (path ? currentFolder : "") + FileNames[CurrentFileIndex] + (bMirrorPlayImage ? "><" : ""));
			if (!bIsRunning && bShowNextFiles) {
				for (int ix = 1; ix < 4; ++ix) {
					if (ix + CurrentFileIndex >= NumberOfFiles) {
						DisplayLine(ix, "");
					}
					else {
						DisplayLine(ix, "   " + FileNames[CurrentFileIndex + ix]);
					}
				}
			}
		}
		else {
			DisplayLine(0, "No SD Card or Files");
		}
	}
	// for debugging keypresses
	//DisplayLine(3, String(nButtonDowns) + " " + nButtonUps);
}

void ShowProgressBar(int percent)
{
	nProgress = percent;
	UpdateBLE(true);
	if (!bShowProgress || bPauseDisplay)
		return;
	static int lastpercent;
	if (lastpercent && (lastpercent == percent))
		return;
	if (percent == 0) {
		OLED->setColor(OLEDDISPLAY_COLOR::BLACK);
		OLED->fillRect(0, 0, OLED->width() - 1, 6);
	}
	OLED->drawProgressBar(0, 0, OLED->width() - 1, 6, percent);
	OLED->display();
	lastpercent = percent;
}

// display message on first line
void WriteMessage(String txt, bool error, int wait)
{
	OLED->clear();
	if (error)
		txt = "**" + txt + "**";
	DisplayLine(0, txt);
	delay(wait);
}

// create the associated IPC name
String MakeIPCFilename(String filename, bool addext)
{
	String cfFile = filename;
	cfFile = cfFile.substring(0, cfFile.lastIndexOf('.'));
	if (addext)
		cfFile += String(".IPC");
	return cfFile;
}

// process the lines in the config file
bool ProcessConfigFile(String filename)
{
	bool retval = true;
	String filepath = currentFolder + filename;
	SDFile rdfile;
	rdfile = SD.open(filepath);
	if (rdfile.available()) {
		//Serial.println("Processing: " + filepath);
		String line, command, args;
		while (line = rdfile.readStringUntil('\n'), line.length()) {
			// read the lines and do what they say
			int ix = line.indexOf('=', 0);
			if (ix > 0) {
				command = line.substring(0, ix);
				command.trim();
				command.toUpperCase();
				args = line.substring(ix + 1);
				// loop through the var list looking for a match
				for (int which = 0; which < sizeof(SettingsVarList) / sizeof(*SettingsVarList); ++which) {
					if (command.compareTo(SettingsVarList[which].name) == 0) {
						switch (SettingsVarList[which].type) {
						case vtInt:
						{
							int val = args.toInt();
							int min = SettingsVarList[which].min;
							int max = SettingsVarList[which].max;
							if (min != max) {
								val = constrain(val, min, max);
							}
							*(int*)(SettingsVarList[which].address) = val;
						}
						break;
						case vtBool:
							args.toUpperCase();
							*(bool*)(SettingsVarList[which].address) = args[0] == 'T';
							break;
						case vtRGB:
						{
							// handle the RBG colors
							CRGB* cp = (CRGB*)(SettingsVarList[which].address);
							cp->r = args.toInt();
							args = args.substring(args.indexOf(',') + 1);
							cp->g = args.toInt();
							args = args.substring(args.indexOf(',') + 1);
							cp->b = args.toInt();
						}
						break;
						default:
							break;
						}
						// we found it, so carry on
						break;
					}
				}
			}
		}
		rdfile.close();
	}
	else
		retval = false;
	return retval;
}

// read the files from the card or list the built-ins
// look for start.IPC, and process it, but don't add it to the list
bool GetFileNamesFromSD(String dir) {
	// start over
	// first empty the current file names
	for (int ix = 0; ix < NumberOfFiles; ++ix) {
		FileNames[ix] = "";
	}
	CurrentFileIndex = 0;
	if (bShowBuiltInTests) {
		for (NumberOfFiles = 0; NumberOfFiles < (sizeof(BuiltInFiles) / sizeof(*BuiltInFiles)); ++NumberOfFiles) {
			FileNames[NumberOfFiles] = BuiltInFiles[NumberOfFiles].text;
		}
	}
	else {
		NumberOfFiles = 0;
		String startfile;
		if (dir.length() > 1)
			dir = dir.substring(0, dir.length() - 1);
		File root = SD.open(dir);
		String CurrentFilename = "";
		if (!root) {
			//Serial.println("Failed to open directory: " + dir);
			return false;
		}
		if (!root.isDirectory()) {
			//Serial.println("Not a directoryf: " + dir);
			return false;
		}

		File file = root.openNextFile();
		if (dir != "/") {
			// add an arrow to go back
			String sdir = currentFolder.substring(0, currentFolder.length() - 1);
			sdir = sdir.substring(0, sdir.lastIndexOf("/"));
			if (sdir.length() == 0)
				sdir = "/";
			FileNames[NumberOfFiles++] = String(PREVIOUS_FOLDER_CHAR) /*+ sdir*/;
		}
		while (file) {
			if (NumberOfFiles >= MAX_FILES) {
				String str = "Max " + String(MAX_FILES) + String(" files allowed");
				WriteMessage(str, true);
				break;
			}
			CurrentFilename = file.name();
			// strip path
			CurrentFilename = CurrentFilename.substring(CurrentFilename.lastIndexOf('/') + 1);
			//Serial.println("name: " + CurrentFilename);
			if (CurrentFilename != "System Volume Information") {
				if (file.isDirectory()) {
					FileNames[NumberOfFiles] = String(NEXT_FOLDER_CHAR) + CurrentFilename;
					//Serial.println("dir: " + CurrentFilename);
					NumberOfFiles++;
				}
				else {
					String uppername = CurrentFilename;
					uppername.toUpperCase();
					if (uppername.endsWith(".BMP")) { //find files with our extension only
						FileNames[NumberOfFiles] = CurrentFilename;
						//Serial.println("file: " + CurrentFilename);
						NumberOfFiles++;
					}
					else if (uppername == "START.IPC") {
						startfile = CurrentFilename;
					}
				}
			}
			file = root.openNextFile();
		}
		root.close();
		delay(500);
		isort(FileNames, NumberOfFiles);
		// see if we need to process the auto start file
		if (startfile.length())
			ProcessConfigFile(startfile);
	}
	return true;
}

// Sort the filenames in alphabetical order
void isort(String* filenames, int n) {
	for (int i = 1; i < n; ++i) {
		String istring = filenames[i];
		int k;
		for (k = i - 1; (k >= 0) && (CompareStrings(istring, filenames[k]) < 0); k--) {
			filenames[k + 1] = filenames[k];
		}
		filenames[k + 1] = istring;
	}
}

// compare two strings, one-two, case insensitive
int CompareStrings(String one, String two)
{
	one.toUpperCase();
	two.toUpperCase();
	return one.compareTo(two);
}

bool SettingsSaveRestore(bool save)
{
	static void* memptr = NULL;
	if (save) {
		// get some memory and save the values
		if (memptr)
			free(memptr);
		memptr = malloc(sizeof saveValueList);
		if (!memptr)
			return false;
	}
	void* blockptr = memptr;
	for (int ix = 0; ix < (sizeof saveValueList / sizeof * saveValueList); ++ix) {
		if (save) {
			memcpy(blockptr, saveValueList[ix].val, saveValueList[ix].size);
		}
		else {
			memcpy(saveValueList[ix].val, blockptr, saveValueList[ix].size);
		}
		blockptr = (void*)((byte*)blockptr + saveValueList[ix].size);
	}
	if (!save) {
		// if it was saved, restore it and free the memory
		if (memptr) {
			free(memptr);
			memptr = NULL;
		}
	}
	return true;
}

void EraseStartFile(MenuItem* menu)
{
	WriteOrDeleteConfigFile("", true, true);
}

void SaveStartFile(MenuItem* menu)
{
	WriteOrDeleteConfigFile("", false, true);
}

void EraseAssociatedFile(MenuItem* menu)
{
	WriteOrDeleteConfigFile(FileNames[CurrentFileIndex].c_str(), true, false);
}

void SaveAssociatedFile(MenuItem* menu)
{
	WriteOrDeleteConfigFile(FileNames[CurrentFileIndex].c_str(), false, false);
}

void LoadAssociatedFile(MenuItem* menu)
{
	String name = FileNames[CurrentFileIndex];
	name = MakeIPCFilename(name, true);
	if (ProcessConfigFile(name)) {
		WriteMessage(String("Processed:\n") + name);
	}
	else {
		WriteMessage(String("Failed reading:\n") + name, true);
	}
}

void LoadStartFile(MenuItem* menu)
{
	String name = "START.IPC";
	if (ProcessConfigFile(name)) {
		WriteMessage(String("Processed:\n") + name);
	}
	else {
		WriteMessage("Failed reading:\n" + name, true);
	}
}

// create the config file, or remove it
// startfile true makes it use the start.IPC file, else it handles the associated name file
bool WriteOrDeleteConfigFile(String filename, bool remove, bool startfile)
{
	bool retval = true;
	String filepath;
	if (startfile) {
		filepath = currentFolder + String("START.IPC");
	}
	else {
		filepath = currentFolder + MakeIPCFilename(filename, true);
	}
	if (remove) {
		if (!SD.exists(filepath.c_str()))
			WriteMessage(String("Not Found:\n") + filepath);
		else if (SD.remove(filepath.c_str())) {
			WriteMessage(String("Erased:\n") + filepath);
		}
		else {
			WriteMessage(String("Failed to erase:\n") + filepath, true);
		}
	}
	else {
		String line;
		File file = SD.open(filepath.c_str(), FILE_WRITE);
		if (file) {
			// loop through the var list
			for (int ix = 0; ix < sizeof(SettingsVarList) / sizeof(*SettingsVarList); ++ix) {
				switch (SettingsVarList[ix].type) {
				case vtInt:
					line = String(SettingsVarList[ix].name) + String(*(int*)(SettingsVarList[ix].address));
					break;
				case vtBool:
					line = String(SettingsVarList[ix].name) + String(*(bool*)(SettingsVarList[ix].address) ? "TRUE" : "FALSE");
					break;
				case vtRGB:
				{
					// handle the RBG colors
					CRGB* cp = (CRGB*)(SettingsVarList[ix].address);
					line = String("WHITE BALANCE=") + String(cp->r) + "," + String(cp->g) + "," + String(cp->b);
				}
				break;
				default:
					line = "";
					break;
				}
				if (line.length())
					file.println(line);
			}
			file.close();
			WriteMessage(String("Saved:\n") + filepath);
		}
		else {
			retval = false;
			WriteMessage(String("Failed to write:\n") + filepath, true);
		}
	}
	return retval;
}

// save some settings in the eeprom
// if autoload is true, check the first flag, and load the rest if it is true
// return true if valid, false if failed
// note that the calvalues must always be read
bool SaveSettings(bool save, bool autoload)
{
	int blockpointer = 0;
	//Serial.println(save ? "saving" : "reading");
	//Serial.println(autoload ? "autoload On" : "autoload off");
	for (int ix = 0; ix < (sizeof saveValueList / sizeof * saveValueList); ++ix) {
		//Serial.println("savesettings ix:" + String(ix));
		if (save) {
			EEPROM.writeBytes(blockpointer, saveValueList[ix].val, saveValueList[ix].size);
		}
		else {  // load
			if (ix == 0) {
				// check signature
				char svalue[sizeof signature];
				EEPROM.readBytes(0, svalue, sizeof(signature));
				//Serial.println("svalue:" + String(svalue));
				if (strncmp(svalue, signature, sizeof signature)) {
					WriteMessage("bad eeprom signature\nSave Default to fix", true);
					return false;
				}
			}
			else {
				EEPROM.readBytes(blockpointer, saveValueList[ix].val, saveValueList[ix].size);
			}
			// if autoload, exit if the save value is not true
			if (autoload && ix == 1) {  // we use 1 here so that the signature and autoload flag are read
				if (!bAutoLoadSettings) {
					return true;
				}
			}
		}
		blockpointer += saveValueList[ix].size;
	}
	if (!save) {
		int savedFileIndex = CurrentFileIndex;
		// we don't know the folder path, so just reset the folder level
		currentFolder = "/";
		setupSDcard();
		CurrentFileIndex = savedFileIndex;
		// make sure file index isn't too big
		if (CurrentFileIndex >= NumberOfFiles) {
			CurrentFileIndex = 0;
		}
	}
	if (save) {
		EEPROM.commit();
		//Serial.println("eeprom committed");
	}
	WriteMessage(String(save ? "Settings Saved" : "Settings Loaded"));
	return true;
}

// save the eeprom settings
void SaveEepromSettings(MenuItem* menu)
{
	SaveSettings(true, false);
}

// load eeprom settings
void LoadEepromSettings(MenuItem* menu)
{
	SaveSettings(false, false);
}

// show some LED's with and without white balance adjust
void ShowWhiteBalance(MenuItem* menu)
{
	for (int ix = 0; ix < 32; ++ix) {
		SetPixel(ix, CRGB(255, 255, 255));
	}
	FastLED.setTemperature(CRGB(255, 255, 255));
	FastLED.show();
	delay(2000);
	FastLED.clear(true);
	delay(50);
	FastLED.setTemperature(CRGB(whiteBalance.r, whiteBalance.g, whiteBalance.b));
	FastLED.show();
	delay(3000);
	FastLED.clear(true);
}

// the bottom strip is reversed
// the top strip is normal
void IRAM_ATTR SetPixel(int ix, CRGB pixel)
{
	if (ix < NUM_LEDS) {
		ix = (NUM_LEDS - 1 - ix);
	}
	if (bUpsideDown) {
		if (bDoublePixels) {
			leds[STRIPLENGTH - 1 - 2 * ix] = pixel;
			leds[STRIPLENGTH - 2 - 2 * ix] = pixel;
		}
		else {
			leds[STRIPLENGTH - 1 - ix] = pixel;
		}
	}
	else {
		if (bDoublePixels) {
			leds[2 * ix] = pixel;
			leds[2 * ix + 1] = pixel;
		}
		else {
			leds[ix] = pixel;
		}
	}
}
