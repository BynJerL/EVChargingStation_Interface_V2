#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <FS.h>
#include <SD.h>
#include <Wire.h>
#include <MFRC522.h>
#include <Bounce2.h>

#define RFID_SCK  14
#define RFID_MISO 12
#define RFID_MOSI 13
#define RFID_SS   15
#define RFID_RST  -1

#define SD_CS 5

#define BUTTON_L 33
#define BUTTON_C 34
#define BUTTON_R 35
#define DOOR_SENSOR 17

#define RELAY_1 27
#define RELAY_2 25
#define RELAY_3 32
#define RELAY_4 26
#define RELAY_5 16

#define MAX_CARDS 50 // Temporary value
#define MAX_UID_LEN 20
#define MAX_NAME_LEN 30

#define BG_COLOR TFT_WHITE
#define TXT_COLOR_1 TFT_BLACK

const unsigned long LOGGED_IN_TIMEOUT = (1 * 60 + 0) * 1000; // 60 seconds
const unsigned long RELAY_ON_TIME = (1 * 60 + 30) * 1000; // 90 seconds
const unsigned long WARNING_TIMEOUT = 10 * 1000; // 10 seconds
const unsigned long CHARGING_SCREEN_TIMEOUT = 2 * 1000; // 2 seconds
const unsigned long int LOADING_SCREEN_TIMEOUT = 2 * 1000; // 2 seconds 

unsigned long int loading_timer = 0;
unsigned long int warning_timer = 0;
int last_menu_index = -1;

bool isScanWaitShow = false;

// RFID Setup
SPIClass hspi(HSPI);
MFRC522 mfrc522(RFID_SS, RFID_RST);

// TFT Display Setup
TFT_eSPI tft = TFT_eSPI();

Bounce l_button;
Bounce c_button;
Bounce r_button;
Bounce door_sensor;

enum Pages {
  SCAN_WAIT,
  SCAN_OK,
  UNAUTHORIZED_CARD,
  CHOOSE_CHARGER,
  CHARGER_ENABLE_CONF,
  CHARGER_ENABLE_SUCCESS,
  DOOR_LOCK,
  CHARGER_DISABLE_CONF,
  CHARGER_DISABLE_SUCCESS,
  LOGOUT_PAGE,
  CHARGER_FULL
};

struct Relay {
  int pin;
  bool state;
  unsigned long timer;
};

struct Card {
  char uid[MAX_UID_LEN];
  char name[MAX_NAME_LEN];
};

Card cardList[MAX_CARDS]; // Array to store card data
int cardCount = 0;

Relay relays[] = {
  {RELAY_1, false, 0}, // Charger 1
  {RELAY_2, false, 0}, // Charger 2
  {RELAY_3, false, 0}, // Battery
  {RELAY_4, false, 0}  // Own Charger
  // {RELAY_5, false, 0}, // Door Lock (Integrated in battery charger)
};

const int relays_count = sizeof(relays) / sizeof(relays[0]);

const char* menu_items[] = {
  "Charger 60V",
  "Charger 72V",
  "Slot Charger",
  "Charger Baterai"
};

String current_uid = "";
int current_uid_index = -1;

String uid_lists[4] = {
  "",
  "",
  "",
  ""
};

int menu_index = 0;
Pages current_page = SCAN_WAIT;
const int menu_items_size = sizeof(menu_items) / sizeof(menu_items[0]);

void loadCardList();

bool isCardScanned();
bool isUID_UsingCharger(String current_uid);
bool isUID_Registered(String current_uid);
bool isSlotAvailable();
bool isBatteryChargerAvailable();

void displayScanWaitMenu();
void displayScanOK_Menu(String current_uid);
void displayUnauthorizedCard();
void displayChargerList();
void displayChargerEnableConf();
void displayChargerEnableSuccess();
void displayDoorLockWaitMenu();
void displayChargerDisableConf();
void displayChargerDisableSuccess();
void displayLogoutMenu();
void displayChargerFull();

void setup() {
  Serial.begin(9600);

  // For safety purpose
  digitalWrite(TFT_CS, HIGH);

  // Buttons and door sensor
  pinMode(BUTTON_L, INPUT_PULLUP);
  pinMode(BUTTON_C, INPUT_PULLUP);
  pinMode(BUTTON_R, INPUT_PULLUP);
  pinMode(DOOR_SENSOR, INPUT_PULLUP);

  // The relays for controlling chargers and door lock
  pinMode(RELAY_1, OUTPUT);
  pinMode(RELAY_2, OUTPUT);
  pinMode(RELAY_3, OUTPUT);
  pinMode(RELAY_4, OUTPUT);
  pinMode(RELAY_5, OUTPUT);

  // Debounce init
  l_button.attach(BUTTON_L, INPUT_PULLUP);
  c_button.attach(BUTTON_C, INPUT_PULLUP);
  r_button.attach(BUTTON_R, INPUT_PULLUP);
  door_sensor.attach(DOOR_SENSOR, INPUT_PULLUP);


  l_button.interval(25);
  c_button.interval(25);
  r_button.interval(25);
  door_sensor.interval(100);

  // TFT display init
  tft.init();
  tft.setRotation(3); // Set rotation, 1 for landscape
  tft.fillScreen(BG_COLOR);

  // SD Card init
  if (!SD.begin(SD_CS)) {
    Serial.println("Card Mount Failed");
    tft.setCursor(10, 10);
    tft.setTextColor(TFT_WHITE);
    tft.setTextSize(1);
    tft.println("Card Mount Failed");
    return;
  }

  Serial.println("SD Card initialized successfully.");

  loadCardList();

  // RFID init
  hspi.begin(RFID_SCK, RFID_MISO, RFID_MOSI, RFID_SS);
  pinMode(RFID_SS, OUTPUT);
  SPI = hspi;
  mfrc522.PCD_Init();

  // displayChargerList();
}

// The loop function is responsible for updating button states and managing the flow of a menu-driven interface based on the current page, handling various states such as waiting for a scan, choosing a charger, and confirming charger enable/disable actions. It includes logic for button presses to navigate and select options within the menu.
void loop() {
  // Control goes here
  for (int i = 0; i < relays_count; i++) {
    if (relays[i].state && millis() - relays[i].timer > RELAY_ON_TIME) {
      relays[i].state = false;
      digitalWrite(relays[i].pin, LOW);
      uid_lists[i] = "";
      if (current_page == CHOOSE_CHARGER) {
        last_menu_index = -1;
        displayChargerList();
      }
    }
  }

  l_button.update();
  c_button.update();
  r_button.update();
  door_sensor.update();

  switch (current_page)
  {
  case SCAN_WAIT:
    // Waiting for Scan Menu
    displayScanWaitMenu();

    if (isCardScanned()) {
      current_uid = "";
      for (byte i = 0; i < mfrc522.uid.size; i++) {
        current_uid += String(mfrc522.uid.uidByte[i], HEX);
      }
      Serial.println("Scanned UID: " + current_uid);

      if (!isUID_Registered(current_uid)) {
        current_page = UNAUTHORIZED_CARD;
        loading_timer = millis();
        isScanWaitShow = false;
        displayUnauthorizedCard();
      } else {
        current_page = SCAN_OK;
        loading_timer = millis();
        isScanWaitShow = false;
        displayScanOK_Menu(current_uid);
      }
    }

    break;
  
  case UNAUTHORIZED_CARD:
    if (millis() - loading_timer <= LOADING_SCREEN_TIMEOUT) {
      return;
    } else {
      current_page = SCAN_WAIT;
      tft.fillScreen(BG_COLOR);
    }
    break;

  case SCAN_OK:
    if (millis() - loading_timer <= LOADING_SCREEN_TIMEOUT) {
      return;
    }

    if (isUID_UsingCharger(current_uid)) {
      current_page = CHARGER_DISABLE_CONF;
      displayChargerDisableConf();

    } else if (!isSlotAvailable()) {
      current_page = CHARGER_FULL;
      displayChargerFull();
      warning_timer = millis();

    } else {
      current_page = CHOOSE_CHARGER;
      menu_index = 0;

    }

    break;

  case CHOOSE_CHARGER:
    displayChargerList();
    // Charger Menu

    if (l_button.fell()) {
      Serial.println("L Button Pressed");
  
      if (menu_index < menu_items_size - 1) {
        menu_index++;
      } else {
        menu_index = 0;
      }
    }
  
    if (c_button.fell()) {
      Serial.println("C Button Pressed");

      if (relays[menu_index].state == false) {
        current_page = CHARGER_ENABLE_CONF;
        displayChargerEnableConf();
      }
      
      // // Toggle the state of the selected charger
      // relays[menu_index].state = !relays[menu_index].state;

      // // Update only the selected menu item to reflect the change
      // int y_position = 30 + menu_index * (50 + 10);
      // int toggle_x = 320 - 140;
      // bool new_state = relays[menu_index].state;

      // // Update Toggle UI
      // tft.fillRect(toggle_x, y_position + 10, 60, 30, new_state ? TFT_DARKGREY : TFT_RED);
      // tft.fillRect(toggle_x + 60, y_position + 10, 60, 30, new_state ? TFT_GREEN : TFT_DARKGREY);
      // tft.setTextColor(TFT_WHITE, new_state ? TFT_GREEN : TFT_DARKGREY);
      // tft.setCursor(toggle_x + 70, y_position + 18);
      // tft.print("ON");
      // tft.setTextColor(TFT_WHITE, new_state ? TFT_DARKGREY : TFT_RED);
      // tft.setCursor(toggle_x + 10, y_position + 18);
      // tft.print("OFF");

      // Serial.print(menu_items[menu_index]);
      // Serial.print(" is now ");
      // Serial.println(new_state ? "ON" : "OFF");
    }
  
    if (r_button.fell()) {
      Serial.println("R Button Pressed");

      if (menu_index > 0) {
        menu_index--;
      } else {
        menu_index = menu_items_size - 1;
      }
    }

    break;
  
  case CHARGER_ENABLE_CONF:
    if (r_button.fell()) {
      current_page = CHOOSE_CHARGER;
      tft.fillScreen(BG_COLOR);
      last_menu_index = -1;
    }

    if (l_button.fell()) {
      relays[menu_index].state = true;
      relays[menu_index].timer = millis();

      digitalWrite(relays[menu_index].pin, relays[menu_index].state);

      uid_lists[menu_index] = current_uid;
      last_menu_index = -1;

      if (menu_index == 3) {
        current_page = DOOR_LOCK;
        displayDoorLockWaitMenu();
        delay(100);
        digitalWrite(RELAY_5, HIGH);

      } else {
        current_page = CHARGER_ENABLE_SUCCESS;
        warning_timer = millis();
        displayChargerEnableSuccess();
      }
    }
    break;
  
  case DOOR_LOCK:

    if (door_sensor.fell()) {
      digitalWrite(RELAY_5, LOW);
      current_page = SCAN_WAIT;
      tft.fillScreen(BG_COLOR);
    }  

    break;
  
  case CHARGER_ENABLE_SUCCESS:
    if (millis() - warning_timer > WARNING_TIMEOUT) {
      current_page = SCAN_WAIT;
      tft.fillScreen(BG_COLOR);
    }

    if (l_button.fell() || c_button.fell() || r_button.fell()) {
      current_page = SCAN_WAIT;
      tft.fillScreen(BG_COLOR);
    }

    break;

  case CHARGER_DISABLE_CONF:
    if (r_button.fell()) {
      current_page = SCAN_WAIT;
      tft.fillScreen(BG_COLOR);
      last_menu_index = -1;
    }

    if (l_button.fell()) {
      relays[current_uid_index].state = false;

      digitalWrite(relays[current_uid_index].pin, relays[current_uid_index].state);

      uid_lists[current_uid_index] = "";
      last_menu_index = -1;

      if (menu_index == 3) {
        current_page = DOOR_LOCK;
        displayDoorLockWaitMenu();
        delay(100);
        digitalWrite(RELAY_5, HIGH);
        
      } else {
        current_page = CHARGER_DISABLE_SUCCESS;
        warning_timer = millis();
        displayChargerDisableSuccess();
      }
    }

    break;
  
  case CHARGER_DISABLE_SUCCESS:
    if (millis() - warning_timer > WARNING_TIMEOUT) {
      current_page = SCAN_WAIT;
      tft.fillScreen(BG_COLOR);
    }

    if (l_button.fell() || c_button.fell() || r_button.fell()) {
      current_page = SCAN_WAIT;
      tft.fillScreen(BG_COLOR);
    }
    break;
  
  case CHARGER_FULL:
    if (millis() - warning_timer > WARNING_TIMEOUT) {
      current_page = SCAN_WAIT;
      tft.fillScreen(BG_COLOR);
    }

    if (l_button.fell() || c_button.fell() || r_button.fell()) {
      current_page = SCAN_WAIT;
      tft.fillScreen(BG_COLOR);
    }

    break;

  default:
    break;
  }
}

void loadCardList() {
  File file = SD.open("/card_list.csv");
  if (!file) {
    Serial.println("Failed to open card_list.csv");
    tft.setCursor(10, 10);
    tft.setTextColor(TFT_WHITE);
    tft.setTextSize(1);
    tft.println("Failed to open card_list.csv");
    return;
  }

  Serial.println("Loading card list...");
  int y = 10; // Initial TFT cursor position

  while (file.available() && cardCount < MAX_CARDS) {
    String line = file.readStringUntil('\n'); // Read one line

    int commaIndex = line.indexOf(','); // Find the comma separator
    if (commaIndex == -1) continue; // Skip invalid lines

    String uid = line.substring(0, commaIndex);
    String name = line.substring(commaIndex + 1);

    // Store in the array
    uid.toCharArray(cardList[cardCount].uid, MAX_UID_LEN);
    name.toCharArray(cardList[cardCount].name, MAX_NAME_LEN);
    cardCount++;

    // Print to Serial Monitor
    Serial.print("UID: "); Serial.print(uid);
    Serial.print(" | Name: "); Serial.println(name);
  }

  file.close();
  Serial.println("Card list loaded.");
}

bool isCardScanned() {
  return mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial(); 
}

bool isUID_UsingCharger(String current_uid) {
  for (int i = 0; i < 4; i++) {
      if (current_uid == uid_lists[i]) {
          current_uid_index = i;
          return true; // Found in the list
      }
  }
  return false; // Not found
}

bool isUID_Registered(String current_uid) {
  for (int i = 0; i < cardCount; i++) {
    if (current_uid == cardList[i].uid) {
      return true;
    }
  }
  return false;
}

bool isSlotAvailable() {
  for (int i = 0; i < 4; i++) {
    if (uid_lists[i] == "") {
        current_uid_index = i;
        return true; 
    }
  }
  return false;
}

bool isBatteryChargerAvailable() {
  
}

void displayScanWaitMenu() {
  // Text properties
  int y_offset = tft.height() / 2 - 20;

  if (!isScanWaitShow) {
    tft.fillScreen(BG_COLOR);
    tft.setTextSize(2);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TXT_COLOR_1, BG_COLOR);
    tft.drawString("Scan Kartu Anda", tft.width() / 2, y_offset);
    tft.drawString("untuk Mulai", tft.width() / 2, y_offset + 30);

    isScanWaitShow = true;
  }

  // Variables for animation
  static unsigned long previousMillis = 0;
  static int animationStep = 0;
  const int animationDelay = 500; // Milliseconds per step

  unsigned long currentMillis = millis();

  // Check if it's time to update animation
  if (currentMillis - previousMillis >= animationDelay) {
    previousMillis = currentMillis;

    // Clear previous dots
    for (int i = 0; i < 3; i++) {
      tft.fillCircle(tft.width() / 2 - 20 + (i * 20), y_offset + 100, 5, TFT_BLACK);
    }

    // Draw new animation step
    tft.fillCircle(tft.width() / 2 - 20 + (animationStep * 20), y_offset + 100, 5, TFT_BLUE);

    // Cycle animation
    animationStep = (animationStep + 1) % 3; // 0 → 1 → 2 → 3 → 0
  }
}

// void displayScanWaitMenu() {
//   tft.fillScreen(TFT_BLACK);

//   // Text properties
//   int y_offset = tft.height() / 2 - 20;
//   tft.setTextSize(2);
//   tft.setTextDatum(MC_DATUM);
//   tft.setTextColor(TFT_WHITE, TFT_BLACK);
//   tft.drawString("Scan Your Card", tft.width() / 2, y_offset);
//   tft.drawString("to Start", tft.width() / 2, y_offset + 30);

//   // Variables for animation
//   unsigned long previousMillis = 0;
//   int animationStep = 0;
//   const int animationDelay = 500; // Milliseconds per step

//   while (true) {
//     unsigned long currentMillis = millis();

//     // Check if it's time to update animation
//     if (currentMillis - previousMillis >= animationDelay) {
//       previousMillis = currentMillis;

//       // Clear previous dots
//       for (int i = 0; i < 3; i++) {
//         tft.fillCircle(tft.width() / 2 - 20 + (i * 20), y_offset + 100, 5, TFT_BLACK);
//       }

//       // Draw new animation step
//       tft.fillCircle(tft.width() / 2 - 20 + (animationStep * 20), y_offset + 100, 5, TFT_BLUE);

//       // Cycle animation
//       animationStep = (animationStep + 1) % 3; // 0 → 1 → 2 → 3 → 0
//     }

//     // Check if a card is scanned
//     if (isCardScanned()) {
//       current_uid = "";

//       for (byte i = 0; i < mfrc522.uid.size; i++) {
//         current_uid += String(mfrc522.uid.uidByte[i], HEX);
//       }
//       Serial.println("Scanned UID: " + current_uid);
//       break; // Exit loop to go to next menu
//     }
//   }

//   // displayScanOK_Menu(); // Placeholder function
//   current_page = SCAN_OK;
//   loading_timer = millis();
//   displayScanOK_Menu(current_uid);
// }

// void displayScanWaitMenu() {
//   tft.fillScreen(TFT_BLACK);
  
//   // Box properties
//   int box_width = 320;   // Full width
//   int box_height = 80;   // Bigger for visibility
//   int x_offset = 0;
//   int y_offset = (tft.height() - box_height) / 2; // Center vertically

//   // Text properties
//   tft.setTextSize(2);
//   tft.setTextDatum(MC_DATUM);
//   tft.setTextColor(TFT_WHITE, TFT_BLACK);
//   tft.drawString("Scan Your Card", tft.width() / 2, y_offset + 20);
//   tft.drawString("to Start", tft.width() / 2, y_offset + 50);

//   // Animated scanning effect
//   for (int i = 0; i < 3; i++) {
//     tft.fillCircle(tft.width() / 2 - 20 + (i * 20), y_offset + box_height + 10, 5, TFT_BLUE);
//     delay(300);
//     tft.fillCircle(tft.width() / 2 - 20 + (i * 20), y_offset + box_height + 10, 5, TFT_BLACK);
//   }  
// }

// void displayScanOK_Menu() {
//   tft.fillScreen(TFT_BLACK);
//   tft.setTextSize(2);
//   tft.setTextDatum(MC_DATUM);
//   tft.setTextColor(TFT_GREEN, TFT_BLACK);
//   tft.drawString("Card Detected!", tft.width() / 2, tft.height() / 2);
// }

void displayScanOK_Menu(String current_uid) {
  tft.fillScreen(BG_COLOR);
  tft.setTextSize(2);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TXT_COLOR_1, BG_COLOR);
  
  // Show "Card Detected!"
  tft.drawString("Card Detected!", tft.width() / 2, tft.height() / 2 - 20);

  // Show UID
  tft.setTextColor(TXT_COLOR_1, BG_COLOR);
  tft.drawString("UID: " + current_uid, tft.width() / 2, tft.height() / 2 + 20);
}

void displayUnauthorizedCard() {
  tft.fillScreen(BG_COLOR);
  tft.setTextSize(2);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TXT_COLOR_1, BG_COLOR);
  tft.drawString("Unauthorized Card!", tft.width() / 2, tft.height() / 2);
}

void displayChargerList() {
  int box_width = 400;   // Full width
  int box_height = 50;   // Increased height for readability
  int x_offset = 30;      // Align left
  int y_offset = 30;     // Start position
  int text_offset = 15;  // Padding for text

  if (last_menu_index == -1) {
    tft.fillScreen(BG_COLOR);

    for (int i = 0; i < menu_items_size; i++) {
      int y_position = y_offset + i * (box_height + 10);
      bool is_selected = (i == menu_index);
      bool is_on = relays[i].state;

      // Menu box
      tft.fillRoundRect(x_offset, y_position, box_width, box_height, 5, is_selected ? TFT_BLUE : TFT_LIGHTGREY);
      // tft.drawRoundRect(x_offset, y_position, box_width, box_height, 5, TFT_WHITE);

      // Menu text
      tft.setTextSize(2);
      tft.setCursor(x_offset + text_offset, y_position + 15);
      tft.setTextColor(is_selected ? TFT_WHITE : TFT_BLACK, is_selected ? TFT_BLUE : TFT_LIGHTGREY);
      tft.print(menu_items[i]);

      // Toggle Indicator
      int toggle_x = x_offset + box_width - 140;
      int toggle_width = 120;
      int toggle_height = 30;

      // Draw toggle box
      tft.fillRoundRect(toggle_x, y_position + 10, toggle_width, toggle_height, 5, TFT_BLACK);
      tft.drawRoundRect(toggle_x, y_position + 10, toggle_width, toggle_height, 5, TFT_WHITE);

      // OFF part
      tft.fillRect(toggle_x, y_position + 10, toggle_width / 2, toggle_height, is_on ? TFT_DARKGREY : TFT_RED);
      tft.setTextColor(TFT_WHITE, is_on ? TFT_DARKGREY : TFT_RED);
      tft.setCursor(toggle_x + 10, y_position + 18);
      tft.print("OFF");

      // ON part
      tft.fillRect(toggle_x + (toggle_width / 2), y_position + 10, toggle_width / 2, toggle_height, is_on ? TFT_GREEN : TFT_DARKGREY);
      tft.setTextColor(TFT_WHITE, is_on ? TFT_GREEN : TFT_DARKGREY);
      tft.setCursor(toggle_x + 10 + (toggle_width / 2), y_position + 18);
      tft.print("ON");
    }
  }

  // Highlight selected menu smoothly
  if (last_menu_index != menu_index) {
    int prev_y = y_offset + last_menu_index * (box_height + 10);
    int new_y = y_offset + menu_index * (box_height + 10);

    if (last_menu_index >= 0) {
      // Reset previous selection
      tft.fillRoundRect(x_offset, prev_y, box_width, box_height, 5, TFT_LIGHTGREY);
      tft.setTextColor(TXT_COLOR_1, TFT_LIGHTGREY);
      tft.setCursor(x_offset + text_offset, prev_y + 15);
      tft.print(menu_items[last_menu_index]);

      // Redraw toggle
      int toggle_x = x_offset + box_width - 140;
      bool prev_state = relays[last_menu_index].state;
      tft.fillRect(toggle_x, prev_y + 10, 60, 30, prev_state ? TFT_DARKGREY : TFT_RED);
      tft.fillRect(toggle_x + 60, prev_y + 10, 60, 30, prev_state ? TFT_GREEN : TFT_DARKGREY);
      tft.setTextColor(TFT_WHITE, prev_state ? TFT_GREEN : TFT_DARKGREY);
      tft.setCursor(toggle_x + 70, prev_y + 18);
      tft.print("ON");
      tft.setTextColor(TFT_WHITE, prev_state ? TFT_DARKGREY : TFT_RED);
      tft.setCursor(toggle_x + 10, prev_y + 18);
      tft.print("OFF");
    }

    // Highlight new selection
    tft.fillRoundRect(x_offset, new_y, box_width, box_height, 5, TFT_BLUE);
    tft.setTextColor(TFT_WHITE, TFT_BLUE);
    tft.setCursor(x_offset + text_offset, new_y + 15);
    tft.print(menu_items[menu_index]);

    // Redraw toggle
    int toggle_x = x_offset + box_width - 140;
    bool new_state = relays[menu_index].state;
    tft.fillRect(toggle_x, new_y + 10, 60, 30, new_state ? TFT_DARKGREY : TFT_RED);
    tft.fillRect(toggle_x + 60, new_y + 10, 60, 30, new_state ? TFT_GREEN : TFT_DARKGREY);
    tft.setTextColor(TFT_WHITE, new_state ? TFT_GREEN : TFT_DARKGREY);
    tft.setCursor(toggle_x + 70, new_y + 18);
    tft.print("ON");
    tft.setTextColor(TFT_WHITE, new_state ? TFT_DARKGREY : TFT_RED);
    tft.setCursor(toggle_x + 10, new_y + 18);
    tft.print("OFF");

    last_menu_index = menu_index;
  }
}


// void displayChargerList() {
//   int box_width = 240;   // Width of the menu item box
//   int box_height = 40;   // Height of the menu item box
//   int x_offset = 10;     // X position of the box
//   int y_offset = 30;     // Starting Y position
//   int text_offset = 10;  // Padding inside the box

//   static int last_menu_index = -1; // Store last selected index

//   // Full menu display (only once)
//   if (last_menu_index == -1) {
//     tft.fillScreen(TFT_BLACK); // Clear screen once

//     for (int i = 0; i < menu_items_size; i++) {
//       int y_position = y_offset + i * (box_height + 10);
      
//       // Background color for each menu item
//       tft.fillRoundRect(x_offset, y_position, box_width, box_height, 5, TFT_BLACK);
      
//       // Print menu text inside the box
//       tft.setTextSize(2);
//       tft.setCursor(x_offset + text_offset, y_position + 10);
//       tft.setTextColor(TFT_WHITE, TFT_BLACK);
//       tft.print(menu_items[i]);

//       // Print the relay status (toggle indicator) to the right
//       tft.setCursor(x_offset + box_width - 40, y_position + 10);
//       if (relays[i].state) {
//         tft.setTextColor(TFT_GREEN, TFT_BLACK);
//         tft.print("ON");
//       } else {
//         tft.setTextColor(TFT_RED, TFT_BLACK);
//         tft.print("OFF");
//       }
//     }
//   }

//   // Update only if selection changes
//   if (last_menu_index != menu_index) {
//     int prev_y = y_offset + last_menu_index * (box_height + 10);
//     int new_y = y_offset + menu_index * (box_height + 10);

//     // Remove previous highlight
//     if (last_menu_index >= 0) {
//       tft.fillRoundRect(x_offset, prev_y, box_width, box_height, 5, TFT_BLACK);
//       tft.setTextColor(TFT_WHITE, TFT_BLACK);
//       tft.setCursor(x_offset + text_offset, prev_y + 10);
//       tft.print(menu_items[last_menu_index]);

//       // Redraw relay status
//       tft.setCursor(x_offset + box_width - 40, prev_y + 10);
//       tft.setTextColor(relays[last_menu_index].state ? TFT_GREEN : TFT_RED, TFT_BLACK);
//       tft.print(relays[last_menu_index].state ? "ON" : "OFF");
//     }

//     // Highlight the new selection
//     tft.fillRoundRect(x_offset, new_y, box_width, box_height, 5, TFT_BLUE);
//     tft.drawRoundRect(x_offset, new_y, box_width, box_height, 5, TFT_WHITE);
//     tft.setTextColor(TFT_WHITE, TFT_BLUE);
//     tft.setCursor(x_offset + text_offset, new_y + 10);
//     tft.print(menu_items[menu_index]);

//     // Redraw relay status
//     tft.setCursor(x_offset + box_width - 40, new_y + 10);
//     tft.setTextColor(relays[menu_index].state ? TFT_GREEN : TFT_RED, TFT_BLUE);
//     tft.print(relays[menu_index].state ? "ON" : "OFF");

//     last_menu_index = menu_index;
//   }
// }


// void displayChargerList() {
//   int box_width = 300;   // Width of the menu item box
//   int box_height = 40;   // Height of the menu item box
//   int x_offset = 10;     // X position of the box
//   int y_offset = 30;     // Starting Y position

//   static int last_menu_index = -1; // Store last selected index

//   tft.setTextSize(2);

//   if (last_menu_index != menu_index) {
//     // Only update previous and new selected item
//     int prev_y = y_offset + last_menu_index * (box_height + 10);
//     int new_y = y_offset + menu_index * (box_height + 10);

//     // Redraw the previously highlighted item
//     if (last_menu_index >= 0) {
//       tft.fillRoundRect(x_offset, prev_y, box_width, box_height, 5, TFT_BLACK); 
//       tft.setTextColor(TFT_WHITE, TFT_BLACK);
//       tft.setCursor(x_offset + 10, prev_y + 10);
//       tft.print(menu_items[last_menu_index]);
//     }

//     // Highlight the new selection
//     tft.fillRoundRect(x_offset, new_y, box_width, box_height, 5, TFT_BLUE);
//     tft.drawRoundRect(x_offset, new_y, box_width, box_height, 5, TFT_WHITE);
//     tft.setTextColor(TFT_WHITE, TFT_BLUE);
//     tft.setCursor(x_offset + 10, new_y + 10);
//     tft.print(menu_items[menu_index]);

//     last_menu_index = menu_index; // Update last menu index
//   }
// }


// void displayChargerList() {
//   tft.fillScreen(TFT_BLACK);
  
//   int box_width = 240;   // Width of the menu item box
//   int box_height = 40;   // Height of the menu item box
//   int x_offset = 10;     // X position of the box
//   int y_offset = 30;     // Starting Y position

//   for (int i = 0; i < menu_items_size; i++) {
//     int y_position = y_offset + i * (box_height + 10); // Space between items

//     // Background color for each menu item
//     if (i == menu_index) {
//       tft.fillRoundRect(x_offset, y_position, box_width, box_height, 5, TFT_BLUE); // Highlighted item
//       tft.drawRoundRect(x_offset, y_position, box_width, box_height, 5, TFT_WHITE); // Border for selected
//       tft.setTextColor(TFT_WHITE, TFT_BLUE);
//     } else {
//       tft.fillRoundRect(x_offset, y_position, box_width, box_height, 5, TFT_BLACK); // Normal item
//       tft.setTextColor(TFT_WHITE, TFT_BLACK);
//     }

//     // Print menu text inside the box with some padding
//     tft.setTextSize(2);
//     tft.setCursor(x_offset + 10, y_position + 10); 
//     tft.print(menu_items[i]);

//     // Print the relay status to the right of the text
//     tft.setCursor(x_offset + box_width - 40, y_position + 10);
    
//     if (relays[i].state) {
//       tft.setTextColor(TFT_GREEN, TFT_BLACK);
//       tft.print("ON");
//     } else {
//       tft.setTextColor(TFT_RED, TFT_BLACK);
//       tft.print("OFF");
//     }
//   }
// }

void displayChargerEnableConf() {
  tft.fillScreen(BG_COLOR);

  int x_offset = 30;
  int y_offset = 30;
  
  // Title
  tft.setTextSize(2);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TXT_COLOR_1, BG_COLOR);
  tft.setCursor(x_offset, y_offset);
  tft.print("Apakah Anda Yakin untuk");
  tft.setCursor(x_offset, y_offset + 30);
  tft.printf("Mengaktifkan [%s]", menu_items[menu_index]);
  tft.setCursor(x_offset, y_offset + 60);
  tft.print("==================================");

  // Instructions
  tft.setTextSize(2);
  tft.setTextColor(TXT_COLOR_1, BG_COLOR);
  tft.setCursor(x_offset, y_offset + 110);
  tft.print("Tekan L untuk Lanjut");
  tft.setCursor(x_offset, y_offset + 140);
  tft.print("Tekan R untuk Kembali");
}

void displayChargerEnableSuccess() {
  tft.fillScreen(BG_COLOR);

  int x_offset = 30;
  int y_offset = 30;
  
  // Message
  tft.setTextSize(2);
  tft.setTextColor(TXT_COLOR_1, BG_COLOR);
  tft.setCursor(x_offset, y_offset);
  tft.printf("Charger Berhasil Diaktifkan!");
  tft.setCursor(x_offset, y_offset + 30);
  tft.print("==================================");

  // Instructions
  tft.setTextSize(2);
  tft.setTextColor(TXT_COLOR_1, BG_COLOR);
  tft.setCursor(x_offset, y_offset + 90);
  tft.print("Tekan tombol apapun untuk Keluar");
}

void displayDoorLockWaitMenu() {
  tft.fillScreen(BG_COLOR);

  int x_offset = 30;
  int y_offset = 30;
  
  // Message
  tft.setTextSize(2);
  tft.setTextColor(TXT_COLOR_1, BG_COLOR);
  tft.setCursor(x_offset, y_offset);
  tft.print("Charger Berhasil ");
  tft.print((relays[4].state)? "Diaktifkan!" : "Dinonaktifkan!");
  tft.setCursor(x_offset, y_offset + 30);
  tft.print("==================================");

  // Instructions
  tft.setTextSize(2);
  tft.setTextColor(TXT_COLOR_1, BG_COLOR);
  tft.setCursor(x_offset, y_offset + 90);
  tft.print("Silahkan ");
  tft.print((relays[4].state)? "masukkan" : "keluarkan");
  tft.print(" baterai Anda");
}

void displayChargerDisableConf() {
  tft.fillScreen(BG_COLOR);
  
  int x_offset = 30;
  int y_offset = 30;

  // Title
  tft.setTextSize(2);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TXT_COLOR_1, BG_COLOR);
  tft.setCursor(x_offset, y_offset);
  tft.print("Apakah Anda Yakin untuk");
  tft.setCursor(x_offset, y_offset + 30);
  tft.printf("menonaktifkan %s", menu_items[current_uid_index]);
  tft.setCursor(x_offset, y_offset + 60);
  tft.print("==================================");

  // Instructions
  tft.setTextSize(2);
  tft.setTextColor(TXT_COLOR_1, BG_COLOR);
  tft.setCursor(x_offset, y_offset + 110);
  tft.print("Tekan L untuk Lanjut");
  tft.setCursor(x_offset, y_offset + 140);
  tft.print("Tekan R untuk Kembali");
}

void displayChargerDisableSuccess() {
  tft.fillScreen(BG_COLOR);
  
  int x_offset = 30;
  int y_offset = 30;

  // Message
  tft.setTextSize(2);
  tft.setTextColor(TXT_COLOR_1, BG_COLOR);
  tft.setCursor(x_offset, y_offset);
  tft.printf("Charger Berhasil Dinonaktifkan!");
  tft.setCursor(x_offset, y_offset + 30);
  tft.print("==================================");

  // Instructions
  tft.setTextSize(2);
  tft.setTextColor(TXT_COLOR_1, BG_COLOR);
  tft.setCursor(x_offset, y_offset + 90);
  tft.print("Tekan tombol apapun untuk Keluar");
}

void displayLogoutMenu() {
  tft.fillScreen(BG_COLOR);

  // Title
  tft.setTextSize(2);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TXT_COLOR_1, BG_COLOR);
  tft.drawString("Terima Kasih", tft.width() / 2, tft.height() / 2);

  // // Instructions
  // tft.setTextSize(2);
  // tft.setTextColor(TFT_WHITE, TFT_BLACK);
  // tft.drawString("Please wait...", tft.width() / 2, tft.height() / 2);
}

void displayChargerFull() {
  tft.fillScreen(BG_COLOR);
  
  int x_offset = 30;
  int y_offset = 30;

  // Message
  tft.setTextSize(2);
  tft.setTextColor(TXT_COLOR_1, BG_COLOR);
  tft.setCursor(x_offset, y_offset);
  tft.printf("Maaf, semua charger sedang digunakan.");
  tft.setCursor(x_offset, y_offset + 30);
  tft.print("==================================");

  // Instructions
  tft.setTextSize(2);
  tft.setTextColor(TXT_COLOR_1, BG_COLOR);
  tft.setCursor(x_offset, y_offset + 90);
  tft.print("Tekan tombol apapun untuk Keluar");
}