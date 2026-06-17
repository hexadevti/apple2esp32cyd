/**
 * A BLE client example that is rich in capabilities.
 * There is a lot new capabilities implemented.
 * author unknown
 * updated by chegewara
 */

// #include "BLEScan.h"
const char keys[] = {
    0x0, 0x0, 0x0, 0x0,
    'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i',
    'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r',
    's', 't', 'u', 'v', 'w', 'x', 'y', 'z',
    '1', '2', '3', '4', '5', '6', '7', '8', '9', '0',
    '\n', 0x0, 0x0, // DEL
    0x0,            // TAB
    ' ',            // SPACE
    '-', '=', '[', ']', '\\', 0x0, ';', '\'', '`', ',', '.', '/',
    0x0, // CAPS LOCK
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x4f, // R_ARROW
    0x0,                                          // L_ARROW
    0x0,                                          // D_ARROW
    0x0,                                          // U_ARROW
    0x0,                                          // NUM LOCK
};

const char shift_keys[] = {
    0x0, 0x0, 0x0, 0x0,
    'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I',
    'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R',
    'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z',
    '!', '@', '#', '$', '%', '^', '&', '*', '(', ')',
    '\n', 0x0, 0x0, // DEL
    0x0,            // TAB
    ' ',            // SPACE
    '_', '+', '{', '}', '|', 0x0, ':', '"', '~', '<', '>', '?',
    0x0, // CAPS LOCK
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x4f, // R_ARROW
    0x0,                                          // L_ARROW
    0x0,                                          // D_ARROW
    0x0,                                          // U_ARROW
    0x0,                                          // NUM LOCK
};

static void notifyCallback(
    BLERemoteCharacteristic *pBLERemoteCharacteristic,
    uint8_t *pData,
    size_t length,
    bool isNotify)
{
#ifdef DEBUG
  // Serial.print("Notify callback for characteristic ");
  // Serial.print(pBLERemoteCharacteristic->getUUID().toString().c_str());
  // Serial.print(" of data length ");
  // Serial.println(length);
  Serial.print("data: ");
  for (size_t i = 0; i < length; i++)
  {
    Serial.printf("%02X", pData[i]);
  }
  Serial.println("");
#endif
  Serial.println("Result char:");
  if (pData[0] == 0x0 && pData[2] != 0x0)
  {
    Serial.printf("%c", keys[pData[2]]);
  }
  else if (pData[0] == 0x02 && pData[2] != 0x0)
  {
    Serial.printf("%c", shift_keys[pData[2]]);
  }
  Serial.println();
}

class MyClientCallback : public BLEClientCallbacks
{
  void onConnect(BLEClient *pclient)
  {
    connected = true;
  }

  void onDisconnect(BLEClient *pclient)
  {
    connected = false;
    Serial.println("onDisconnect");
    doScan = true;
  }
};

bool connectToServer()
{
  Serial.print("Forming a connection to ");
  Serial.println(myDevice->getAddress().toString().c_str());

  BLEClient *pClient = BLEDevice::createClient();
  Serial.println(" - Created client");

  pClient->setClientCallbacks(new MyClientCallback());

  // Connect to the remove BLE Server.
  if (!pClient->connect(myDevice))
  { // if you pass BLEAdvertisedDevice instead of address, it will be recognized type of peer device address (public or private)
    return false;
  }
  Serial.println(" - Connected to server");

  // Obtain a reference to the service we are after in the remote BLE server.
  BLERemoteService *pRemoteService = pClient->getService(serviceUUID);
  if (pRemoteService == nullptr)
  {
    Serial.print("Failed to find our service UUID: ");
    Serial.println(serviceUUID.toString().c_str());
    pClient->disconnect();
    return false;
  }
  Serial.println(" - Found our service");

  // Obtain a reference to the characteristic in the service of the remote BLE server.
  do
  {
    pRemoteCharacteristic = pRemoteService->getCharacteristic(charUUID);
    if (pRemoteCharacteristic == nullptr)
    {
      Serial.print("Failed to find our characteristic UUID: ");
      Serial.println(charUUID.toString().c_str());
      pClient->disconnect();
      return false;
    }

    if (pRemoteCharacteristic->canNotify())
      break;
  } while (1);
  Serial.println(" - Found our characteristic");

  pRemoteCharacteristic->registerForNotify(notifyCallback);

  return true;
}
/**
 * Scan for BLE servers and find the first one that advertises the service we are looking for.
 */
class MyAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks
{
  /**
   * Called for each advertising BLE server.
   */
  void onResult(BLEAdvertisedDevice advertisedDevice)
  {
    Serial.print("BLE Advertised Device found: ");
    Serial.println(advertisedDevice.toString().c_str());

    // We have found a device, let us now see if it contains the service we are looking for.
    if (advertisedDevice.haveServiceUUID() && advertisedDevice.isAdvertisingService(serviceUUID))
    {

      BLEDevice::getScan()->stop();
      myDevice = new BLEAdvertisedDevice(advertisedDevice);
      doConnect = true;
      doScan = true;

    } // Found our server
  } // onResult
}; // MyAdvertisedDeviceCallbacks

void bleSetup()
{

  Serial.println("Starting Arduino BLE Client application...");
  BLEDevice::init("");

  // Retrieve a Scanner and set the callback we want to use to be informed when we
  // have detected a new device.  Specify that we want active scanning and start the
  // scan to run for 5 seconds.
  BLEScan *pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setInterval(1349);
  pBLEScan->setWindow(449);
  pBLEScan->setActiveScan(true);
  pBLEScan->start(5, false);
  xTaskCreate(
      bleConnect, "bleConnect", 4096, NULL, 1, NULL);

} // End of setup.

void bleConnect(void *pvParameters)
{
  while (true)
  {
    Serial.printf("doconnect: %d, connected: %d, doScan: %d\n", doConnect, connected, doScan);
    if (doConnect)
    {
      if (connectToServer())
      {
        Serial.println("We are now connected to the BLE Server.");
        doScan = false;
      }
      else
      {
        Serial.println("We have failed to connect to the server; there is nothin more we will do.");
      }
      doConnect = false;
    }
    else if (doScan)
    {
      BLEDevice::getScan()->start(5, false); // this is just eample to start scan after disconnect, most likely there is better way to do it in arduino
    }

    // if (connected)
    // {

    // }
    // else if (doScan)
    // {
    //     BLEDevice::getScan()->start(0); // this is just eample to start scan after disconnect, most likely there is better way to do it in arduino
    // }

    delay(1000); // Delay a second between loops.
  }

} // End of loop