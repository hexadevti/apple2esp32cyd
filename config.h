// apple2esp32cyd - Apple II emulator for the ESP32 Cheap Yellow Display
// (ESP32 + ILI9341 TFT via TFT_eSPI), SD-card storage.


#include "FS.h"
#include <SPI.h>
#include <TFT_eSPI.h>
#include "SD.h"
#include <EEPROM.h>
#include "rom.h"
#include <string>
#include <bitset>
#include <algorithm>
#include <array>
#include <cmath>
#include <queue>
#include <thread>
#include <mutex>
#include <vector>
#include <deque>
#include <map>
#include <condition_variable>

#include "BLEDevice.h"


// BLE

// The remote service we wish to connect to.
static BLEUUID serviceUUID((uint16_t) 0x1812);
// The characteristic of the remote service we are interested in.
static BLEUUID    charUUID((uint16_t) 0x2a4d);
static bool doConnect = false;
static bool connected = false;
static bool doScan = false;
static BLERemoteCharacteristic* pRemoteCharacteristic;
static BLEAdvertisedDevice* myDevice;


size_t content_len;
File file;
bool opened = false;
static String filelist = "";

static int freeSpace = 0;



// SD storage

#define FSTYPE SD


// SD Config
std::vector<std::string> hdFiles;
std::vector<std::string> diskFiles;

// Board Pins
 
    #define SD_SCK_PIN 18
    #define SD_MISO_PIN 19
    #define SD_MOSI_PIN 23
    #define SD_CS_PIN 5
    #define KEYBOARD_DATA_PIN 21
    #define KEYBOARD_IRQ_PIN 22
    #define ANALOG_X_PIN 4
    #define ANALOG_Y_PIN 35
    #define LED_PIN 17
    #define DIGITAL_BUTTON12_PIN 34 // joystick buttons 0-3
    #define SPEAKER_PIN 26
//#define RED_LED_PIN 4
//#define BLUE_LED_PIN 16 // unused

// #define GPIO25 25 // unused
// #define GPIO32 32 // unused
// #define TFT_MISO 12
// #define TFT_MOSI 13 // In some display driver board, it might be written as "SDA" and so on.
// #define TFT_SCLK 14
// #define TFT_CS   15  // Chip select control pin
// #define TFT_DC   2  // Data Command control pin
// #define TFT_BL   27  // LED back-light
// #define TOUCH_CS 33     // Chip select pin (T_CS) of touch screen


// Video Config
TFT_eSPI tft = TFT_eSPI();
int margin_x = 20;
int margin_y = 24;

static const uint16_t screenWidth  = 240;
static const uint16_t screenHeight = 320;
static std::mutex page_lock;


uint16_t tx = 0, ty = 0; // To store the touch coordinates


// keyboard
// keyboard scan buffer
unsigned short keyboard_data[3] = {0, 0, 0};
unsigned char keyboard_buf_indx = 0, keyboard_mbyte = 0;
boolean shift_enabled = false;
boolean ctrl_enabled = false;
static char keymem = 0;

// Joystick Config

#define JOY_MAX 1024
#define JOY_MID 512
#define JOY_MIN 0
#define EEPROM_SIZE 1024
static int fnSelected = 0;
int joystickCycles0 = 0;
int joystickCycles1 = 0;
int joystickCycles2 = 0;
int joystickCycles3 = 0;

int analogX = 0;
int analogY = 0;
int digital_button1;

bool pPb0 = false;
bool pPb1 = false;
bool pPb2 = false;
bool pPb3 = false;


int joyX = 1;
int joyY = 1;
int pJoyX = 1;
int pJoyY = 1;

// Fetures default Config
static bool running = true;
static bool paused = false;
static bool sound = true;
static bool dacSound = false;
static bool upscale = false;
static bool smoothUpscale = false;
static bool AppleIIe = true;
static bool OptionsWindow = false;
static bool DebugWindow = false;
static bool ClearWindow = false;
static bool initializedHdDisk = false;
static bool HdDisk = false;
static bool Fast1MhzSpeed = true;
static bool joystick = true;
static bool mouse = true;
static bool diskAttached = false;
static bool hdAttached = false;
static bool serialVideoAttached = false;
static bool serialKeyboardAttached = false;
static bool videoColor = true;
static bool wifiConnected = false;
static uint8_t volume = 0x40;

// Target system for the multi-platform emulator. Apple II is implemented; C64 and
// NES are placeholders selectable from the boot splash (see video.ino splashService
// and the dispatch in apple2esp32cyd.ino). Persisted in EEPROM.
enum Platform : uint8_t { PLATFORM_APPLE2 = 0, PLATFORM_C64 = 1, PLATFORM_NES = 2 };
static uint8_t currentPlatform = PLATFORM_APPLE2;

// Log Config
char buf[0xff];
int logLineCount = 1;

// EPROM Config
#define HdDiskEEPROMaddress 0
#define IIpIIeEEPROMaddress 1
#define Fast1MhzSpeedEEPROMaddress 2
#define JoystickEEPROMaddress 3
#define VideoColorEEPROMaddress 4
#define SoundEEPROMaddress 5
#define VolumeEEPROMaddress 6
#define dacSoundEEPROMaddress 7
#define PlatformEEPROMaddress 8
#define NewDeviceConfigEEPROMaddress 50
#define DiskFileNameEEPROMaddress 128
#define HdFileNameEEPROMaddress 256
String selectedDiskFileName;
String selectedHdFileName;
String NewDeviceConfig;
byte selectedHdFile;
uint8_t firstShowFile = 0;
uint8_t shownFile = 0xff;

// Softswitches Config
static bool Graphics_Text = false;
static bool Page1_Page2 = true;
static bool DisplayFull_Split = true;
static bool LoRes_HiRes = true;
static bool Cols40_80 = true;
static bool lock_video = false;
static bool IntCXRomOn_Off = false;
static bool IntC8RomOn_Off = false;
static bool AltCharSetOn_Off = false;
static bool SlotC3RomOn_Off = false;
static bool Store80On_Off = false;
static bool Vertical_blankingOn_Off = false;
static bool RAMReadOn_Off = false;
static bool RAMWriteOn_Off = false;
static bool AltZPOn_Off = false;
static bool IOUDisOn_Off = true;
static bool DHiResOn_Off = false;
static bool IIEMemoryBankBankSelect1_2 = true;
static bool IIEMemoryBankReadRAM_ROM = false;
static bool IIEMemoryBankWriteRAM_NoWrite = false;
static uint8_t IIeExpansionCardBank = 0;
static bool MemoryBankBankSelect1_2 = true;
static bool MemoryBankReadRAM_ROM = false;
static bool MemoryBankWriteRAM_NoWrite = false;
static bool diskUnitNumber1_2 = true;
static bool DrivePhase0ON_OFF;
static bool DrivePhase1ON_OFF;
static bool DrivePhase2ON_OFF;
static bool DrivePhase3ON_OFF;
static bool FlagDO_PO;
static bool DriveQ6H_L;
static bool DriveQ7H_L;
static bool DriveMotorON_OFF;


// Memory Config
static unsigned char zp[0x200];
static unsigned char auxzp[0x200];
static unsigned char* ram;
static unsigned char* auxram;
static unsigned char* memoryBankSwitchedRAM1;
static unsigned char* memoryBankSwitchedRAM2_1;
static unsigned char* memoryBankSwitchedRAM2_2;
static unsigned char* IIEAuxBankSwitchedRAM1;
static unsigned char* IIEAuxBankSwitchedRAM2_1;
static unsigned char* IIEAuxBankSwitchedRAM2_2;
static unsigned char* IIEmemoryBankSwitchedRAM1;
static unsigned char* IIEmemoryBankSwitchedRAM2_1;
static unsigned char* IIEmemoryBankSwitchedRAM2_2;
static unsigned char* menuScreen;
static unsigned char* menuColor;

//Speaker Config

boolean speaker_state = false;

// Other Cofig
static bool CgReset0 = false;
static bool CgReset1 = false;
static bool CgReset2 = false;
static bool CgReset3 = false;
static bool Cg0 = false;
static bool Cg1 = false;
static bool Cg2 = false;
static bool Cg3 = false;
static float timerpdl0 = 0;
static float timerpdl1 = 0;
static float timerpdl2 = 0;
static float timerpdl3 = 0;
static bool Pb0 = false;
static bool Pb1 = false;
static bool Pb2 = false;
static bool Pb3 = false;

// Mouse
static int16_t mouseX = 0;
static int16_t mouseY = 0;
static bool mouseButton = false;