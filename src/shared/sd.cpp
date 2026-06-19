#include "../../emu.h"

static SPIClass hspi { HSPI };

void FSSetup()
{
  hdAttached = HdDisk;
  diskAttached = !HdDisk;
  Serial.println("SD Card Setup");
  
  int sdMountRetry = 0;
      hspi.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
      // Run the SD bus at SD_SPI_HZ (20MHz) instead of the begin() default of 4MHz: directory
      // reads (esp. nested opendir) were the bottleneck after the per-entry fopen was removed.
      while (!FSTYPE.begin(SD_CS_PIN, hspi, SD_SPI_HZ) && sdMountRetry < 10) {
        printLog("Card Mount Failed");
        delay(100);
        sdMountRetry++;
      }

      if (sdMountRetry == 10) {
        hdAttached = false;
        diskAttached = false;
        return;
      }
      


  uint8_t cardType = FSTYPE.cardType();

  if (cardType == CARD_NONE) {
    printLog("No SD card attached");
    return;
  }

  printLog("SD Card Type: ");
  if (cardType == CARD_MMC) {
    printLog("MMC");
  } else if (cardType == CARD_SD) {
    printLog("SDSC");
  } else if (cardType == CARD_SDHC) {
    printLog("SDHC");
  } else {
    printLog("UNKNOWN");
  }
  
  uint64_t cardSize = FSTYPE.cardSize() / (1024 * 1024);
  sprintf(buf,"SD Card Size: %lluMB\n", cardSize);
  printLog(buf);


}