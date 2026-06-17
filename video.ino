

const uint16_t colors[8] = {TFT_BLACK, TFT_GREEN, TFT_PURPLE, TFT_WHITE, TFT_BLACK, tft.color565(255, 20, 0), TFT_SKYBLUE, TFT_WHITE};
const uint16_t colors16[16] = {tft.color565(0, 0, 0), tft.color565(147, 11, 124), tft.color565(98, 76, 0), tft.color565(249, 86, 29),
                                       tft.color565(0, 118, 12), tft.color565(126, 126, 126), tft.color565(67, 200, 0), tft.color565(220, 205, 22),
                                       tft.color565(31, 53, 211), tft.color565(187, 54, 255), tft.color565(126, 126, 126), tft.color565(255, 129, 236),
                                       tft.color565(7, 168, 224), tft.color565(157, 172, 255), tft.color565(93, 247, 132), tft.color565(255, 255, 255)};
int flashCount = 0;
int touchCount = 0;
int width = 280;
int height = 192;

void videoSetup()
{
  printLog("Video Setup...");
  tft.begin();
  tft.setRotation(3);
  tft.invertDisplay(true);
  tft.initDMA();
  tft.fillRect(0, 0, 320, 240, TFT_BLACK);
  xTaskCreatePinnedToCore(renderLoop, "renderLoop", 4096, NULL, 1, NULL, 0); // core 0; keep core 1 for the CPU
}

int red(int color) {
  return (color & 0xf800) >> 8;
}
int green(int color) {
  return (color & 0x7e0) >> 3;
}
int blue(int color) {
  return (color & 0x1f) << 3;
}

bool inversed = false;
float screen_width = 280;
float screen_height = 192;
uint16_t last_y = 0;
uint16_t last_x = 0;

void renderLoop(void *pvParameters)
{
  
  while (running)
  {
    Vertical_blankingOn_Off = false;
    unsigned long startTime = millis();

    // 320x240 TFT: center the 280x192 raster (overriding the S3/VGA margins below).
    margin_x = 20;
    margin_y = 24;
    screen_width = 280;
    screen_height = 192;
    last_y = margin_y;
    last_x = margin_x;

    float coef192 = screen_height / 192;
    float coef560 = screen_width / 560;
    float coef280 = screen_width / 280;
    float coef140 = screen_width / 140;

    if (!OptionsWindow && AppleIIe && !Cols40_80 && !DHiResOn_Off)
    tft.setAddrWindow(0, margin_y, 320, 192); // Set the area to draw
    else if (!OptionsWindow && AppleIIe && DHiResOn_Off && !videoColor)
    tft.setAddrWindow(margin_x, margin_y, 280, 192); // DHiRes mono (centered 280)
    else if (OptionsWindow || clearScr)
    tft.setAddrWindow(2, 0, 315, 240);
    else
    tft.setAddrWindow(margin_x, margin_y, 280, 192);
    tft.startWrite();
    
    

    int x = margin_x;
    int y = margin_y;
    int x_upscaled = margin_x;
    int y_upscaled = margin_y;
    // Snapshot the page selection under a brief lock so a CPU page-flip (C054/C055)
    // can't block for a whole frame; the rest of the frame renders from these locals.
    page_lock.lock();
    ushort textPage = Page1_Page2 ? 0x400 : 0x800;
    ushort graphicsPage = Page1_Page2 ? 0x2000 : 0x4000;
    page_lock.unlock();
    unsigned long endTime1 = millis();
    // if (demo) {
    // y=0;
    //   for (int v = 0; v < 30; v++)
    //   {
    //     for (int i = 0; i < 8; i++) // char lines
    //     {
    //       x=0;
    //       for (int h = 0; h < 45; h++)
    //       {
    //         uint8_t chr = menuScreen[v * 45 + h];
    //         for (int c = 0; c < 7; c++) // char cols
    //         {
    //           bool bpixel = AppleIIeFontPixels[(chr*7*8) + (i * 7) + c];
    //           uint8_t color = menuColor[v * 45 + h];
    //           uint8_t fgColor = 0xff;
    //           uint8_t bgColor = color;
    //           #ifdef TFT
    //           tft.writeColor((bpixel ? fgColor : bgColor), 1);
    //           #else
    //           vga.dotFast(x, y, bpixel ? fgColor : bgColor);
    //           x++;
    //           vga.dotFast(x, y, bpixel ? fgColor : bgColor);
    //           #endif
    //           x++;
    //         }
    //       }
    //       y++;
    //     }
    //   }
    // }
    if (clearScr) {
      y=0;
      for (int v = 0; v < 30; v++)
      {
        for (int i = 0; i < 8; i++) // char lines
        {
          x=0;
          for (int h = 0; h < 45; h++)
          {
            for (int c = 0; c < 7; c++) // char cols
            {
              tft.writeColor(colors[0], 1);
              x++;
            }
          }
          y++;
        }
      }
      clearScr = false;
    }
    else if (OptionsWindow || DebugWindow) 
    {
      y=0;
      for (int v = 0; v < 30; v++)
      {
        for (int i = 0; i < 8; i++) // char lines
        {
          x=0;
          for (int h = 0; h < 45; h++)
          {
            uint8_t chr = menuScreen[v * 45 + h];
            for (int c = 0; c < 7; c++) // char cols
            {
              bool bpixel = AppleIIeFontPixels[(chr*7*8) + (i * 7) + c];
              uint8_t color = menuColor[v * 45 + h];
              uint8_t fgColor = (color & 0xf0) >> 4;
              uint8_t bgColor = (color & 0x0f);
                tft.writeColor((bpixel ? colors16[fgColor] : colors16[bgColor]), 1);
              x++;
            }
          }
          y++;
        }
      }
    }
    else
    {
      for (int b = 0; b < 3; b++)
      {
        for (int l = 0; l < 8; l++)
        {
          if ((Graphics_Text && DisplayFull_Split) || (Graphics_Text && !DisplayFull_Split && (b < 2 || (b == 2 && l < 4))))
          {
            if (LoRes_HiRes)
            {
              for (int j = 0; j < 8; j++)
              {
                // Init Upscale
                int repeat_y = 0;
                uint16_t upscaleCoef_y = floor(coef192 * (float)(y+1));
                uint16_t repeatTimes_y = upscaleCoef_y - last_y;
                last_y = upscaleCoef_y;
                while (repeat_y < repeatTimes_y)
                {
                  x_upscaled = margin_x;
                  x = margin_x;
                  last_x = margin_x;
                  uint16_t lastPixel = 0;
                  bool lastLine = false;
                  bool lastCol = false;
                  // End Upscale
                  for (int c = 0; c < 0x28; c++)
                  {
                    for (int k = 0; k < 7; k++)
                    {
                      char value = ram[(textPage + (b * 0x28) + (l * 0x80) + c)];
                      int firstColor = (value & 0b11110000) >> 4;
                      int secondColor = value & 0b00001111;
                        if (j < 4)
                          tft.writeColor(colors16[secondColor], 1);
                        else
                          tft.writeColor(colors16[firstColor], 1);
                      
                      x++;
                    }
                  }
                  repeat_y++;
                  y_upscaled++;
                }
                y++;
              }
            }
            else if (DHiResOn_Off)
            {
              static bool line[0x50 * 7]; // 560 DHGR bits; static avoids per-scanline malloc churn

              for (int block = 0; block < 8; block++)
              {
                // Init Upscale
                int repeat_y = 0;
                uint16_t upscaleCoef_y = floor(coef192 * (float)(y+1));
                //Serial.printf("upscaleCoef_y=%d screen_width=%f (float)(y+1)=%f last_y=%d\n",upscaleCoef_y, screen_width, (float)(y+1), last_y);
                uint16_t repeatTimes_y = upscaleCoef_y - last_y;
                //Serial.printf("y=%d repeatTimes_y=%d\n", y, repeatTimes_y);
                last_y = upscaleCoef_y;
                while (repeat_y < repeatTimes_y)
                {
                  int lineId = 0;
                  x_upscaled = margin_x;
                  x = margin_x;
                  last_x = margin_x;
                  uint16_t lastPixel = 0;
                  bool lastLine = false;
                  bool lastCol = false;
                  char bottomChr;
                  // End Upscale
                  for (ushort c = 0; c < 0x50; c++)
                  {
                    char chr;
                    if (c % 2 == 0)
                    {
                      chr = auxram[(ushort)((0x2000 + (b * 0x28) + (l * 0x80) + c / 2) + block * 0x400)];
                    }
                    else
                    {
                      chr = ram[(ushort)((0x2000 + (b * 0x28) + (l * 0x80) + (c - 1) / 2) + block * 0x400)];
                    }

                    bool blockline[8];
                    for (int i = 0; i < 8; i++)
                      blockline[7 - i] = (chr & (1 << i)) != 0;

                    bool fillLine = videoColor;
                    fillLine = true; // TFT renders both color & mono from line[] after the row
                    if (fillLine)
                    {
                      for (int i = 7; i > 0; i--)
                      {
                        *(line + lineId) = blockline[i];
                        lineId++;
                      }
                    }
                    else
                    {
                      for (int i = 7; i > 0; i--)
                      {
                        // Init Upscale
                        int repeat_x = 0;
                        uint16_t upscaleCoef_x = floor(coef560 * (float)(x+1));
                        //Serial.printf("upscaleCoef_x=%d screen_width=%f (float)(x+1)=%f last_x=%d\n",upscaleCoef_x, screen_width, (float)(x+1), last_x);
                        bool downScale = upscaleCoef_x < last_x;
                        uint8_t repeatTimes_x = upscaleCoef_x - last_x;
                        //Serial.printf("x=%d repeatTimes_x=%d\n", x, repeatTimes_x);
                        last_x = upscaleCoef_x;
                        while (repeat_x < repeatTimes_x)
                        {
                          repeat_x++;
                        }
                        x++;
                      }
                    }
                  }
                  if (videoColor) {
                    // 140 DHGR color pixels (4 bits each) x2 = 280 px to match the window
                    for (int i = 0; i < 0x50 * 7; i += 4) {
                      int color = (line[i] ? 8 : 0) + (line[i + 1] ? 4 : 0) + (line[i + 2] ? 2 : 0) + (line[i + 3] ? 1 : 0);
                      tft.writeColor(colors16[color], 2);
                    }
                  } else {
                    // 560 mono DHGR bits downsampled 2:1 = 280 px to match the window
                    for (int i = 0; i < 0x50 * 7; i += 2)
                      tft.writeColor(line[i] ? TFT_WHITE : TFT_BLACK, 1);
                  }
                  repeat_y++;
                  y_upscaled++;
                }
                y++;
              }

            }
            else // hires
            {
              for (int block = 0; block < 8; block++)
              {
                // Track the displayed page live (C054/C055) instead of latching it once
                // per frame: with a slow free-running renderer the latched page can be the
                // one the CPU is redrawing during page-flip animation, which flickers.
                // Reading it per scanline keeps us on the currently-shown (completed) page.
                graphicsPage = Page1_Page2 ? 0x2000 : 0x4000;
                // Init Upscale
                int repeat_y = 0;
                uint16_t repeatTimes_y = 1;
                uint16_t upscaleCoef_y = floor(coef192 * (float)(y+1));
                repeatTimes_y = upscaleCoef_y - last_y;
                last_y = upscaleCoef_y;
                while (repeat_y < repeatTimes_y)
                {
                  x_upscaled = margin_x;
                  x = margin_x;
                  last_x = margin_x;
                  uint16_t lastPixel = 0;
                  bool lastLine = false;
                  bool lastCol = false;
                  // End Upscale
                  for (ushort c = 0; c < 0x28; c++)
                  {
                    if (c == 0x27)
                      lastCol = true;
                    char chr;
                    char prevChr;
                    char pixels[7];
                    char chrBottom;
                    char prevChrBottom;
                    char pixelsBottom[7];
                    if (smoothUpscale) {
                      if (videoColor && repeat_y > 0) {
                        int blockb = block;
                        int lb = l;
                        int bb = b;
                        blockb++;
                        if (blockb == 8) { blockb=0; lb++; }
                        if (lb == 8) { lb=0; bb++; }
                        if (bb == 3) { bb=0; lastLine = true; }

                        chrBottom = ram[(ushort)(((graphicsPage) + (bb * 0x28) + (lb * 0x80) + c) + blockb * 0x400)];
                        if (c % 2 == 0) // Odd
                        {
                          pixelsBottom[0] = (chrBottom & 0x80) >> 5 | (chrBottom & 1) << 1 | (prevChrBottom & 0x40) >> 6;
                          pixelsBottom[1] = (chrBottom & 0x80) >> 5 | (chrBottom & 1) << 1 | (chrBottom & 0x2) >> 1;
                          pixelsBottom[2] = (chrBottom & 0x80) >> 5 | (chrBottom & 0x4) >> 1 | (chrBottom & 0x2) >> 1;
                          pixelsBottom[3] = (chrBottom & 0x80) >> 5 | (chrBottom & 0x4) >> 1 | (chrBottom & 0x8) >> 3;
                          pixelsBottom[4] = (chrBottom & 0x80) >> 5 | (chrBottom & 0x10) >> 3 | (chrBottom & 0x8) >> 3;
                          pixelsBottom[5] = (chrBottom & 0x80) >> 5 | (chrBottom & 0x10) >> 3 | (chrBottom & 0x20) >> 5;
                          pixelsBottom[6] = (chrBottom & 0x80) >> 5 | (chrBottom & 0x40) >> 5 | (chrBottom & 0x20) >> 5;
                        }
                        else // Even
                        {
                          pixelsBottom[0] = (chrBottom & 0x80) >> 5 | (prevChrBottom & 0x40) >> 5 | (chrBottom & 0x1); 
                          pixelsBottom[1] = (chrBottom & 0x80) >> 5 | (chrBottom & 0x2) | (chrBottom & 0x1);
                          pixelsBottom[2] = (chrBottom & 0x80) >> 5 | (chrBottom & 0x2) | (chrBottom & 0x4) >> 2;
                          pixelsBottom[3] = (chrBottom & 0x80) >> 5 | (chrBottom & 0x8) >> 2 | (chrBottom & 0x4) >> 2;
                          pixelsBottom[4] = (chrBottom & 0x80) >> 5 | (chrBottom & 0x8) >> 2 | (chrBottom & 0x10) >> 4;
                          pixelsBottom[5] = (chrBottom & 0x80) >> 5 | (chrBottom & 0x20) >> 4 | (chrBottom & 0x10) >> 4;
                          pixelsBottom[6] = (chrBottom & 0x80) >> 5 | (chrBottom & 0x20) >> 4 | (chrBottom & 0x40) >> 6;
                          
                        }
                      }
                    }
                    
                    chr = ram[(ushort)(((graphicsPage) + (b * 0x28) + (l * 0x80) + c) + block * 0x400)];

                    if (videoColor)
                    {
                      char pixels[7];
                      if (c % 2 == 0) // Odd
                      {
                        pixels[0] = (chr & 0x80) >> 5 | (chr & 1) << 1 | (prevChr & 0x40) >> 6;
                        pixels[1] = (chr & 0x80) >> 5 | (chr & 1) << 1 | (chr & 0x2) >> 1;
                        pixels[2] = (chr & 0x80) >> 5 | (chr & 0x4) >> 1 | (chr & 0x2) >> 1;
                        pixels[3] = (chr & 0x80) >> 5 | (chr & 0x4) >> 1 | (chr & 0x8) >> 3;
                        pixels[4] = (chr & 0x80) >> 5 | (chr & 0x10) >> 3 | (chr & 0x8) >> 3;
                        pixels[5] = (chr & 0x80) >> 5 | (chr & 0x10) >> 3 | (chr & 0x20) >> 5;
                        pixels[6] = (chr & 0x80) >> 5 | (chr & 0x40) >> 5 | (chr & 0x20) >> 5;
                      }
                      else // Even
                      {
                        pixels[0] = (chr & 0x80) >> 5 | (prevChr & 0x40) >> 5 | (chr & 0x1); 
                        pixels[1] = (chr & 0x80) >> 5 | (chr & 0x2) | (chr & 0x1);
                        pixels[2] = (chr & 0x80) >> 5 | (chr & 0x2) | (chr & 0x4) >> 2;
                        pixels[3] = (chr & 0x80) >> 5 | (chr & 0x8) >> 2 | (chr & 0x4) >> 2;
                        pixels[4] = (chr & 0x80) >> 5 | (chr & 0x8) >> 2 | (chr & 0x10) >> 4;
                        pixels[5] = (chr & 0x80) >> 5 | (chr & 0x20) >> 4 | (chr & 0x10) >> 4;
                        pixels[6] = (chr & 0x80) >> 5 | (chr & 0x20) >> 4 | (chr & 0x40) >> 6;
                      }

                      for (int id = 0; id < 7; id++)
                      { 
                        tft.writeColor(colors[pixels[id]], 1);
                        x++;
                      }
                      prevChr = chr;
                      if (smoothUpscale) {
                        if (repeat_y > 0) {
                          prevChrBottom = chrBottom;
                        }
                      }
                    }
                    else
                    {
                      bool blockline[8];
                      for (int i = 0; i < 8; i++)
                        blockline[7 - i] = (chr & (1 << i)) != 0;
                      for (int i = 7; i > 0; i--)
                      {
                        uint16_t color = TFT_BLACK;
                        if (blockline[i])
                          color = TFT_WHITE;
                        else
                          color = TFT_BLACK;
                        tft.writeColor(color, 1);
                        x++;
                      }
                    }
                  }
                  repeat_y++;
                  y_upscaled++;
                }
                y++;
              }
            }
          }
          else if (Cols40_80) // Text modes
          {
            for (int i = 0; i < 8; i++) // char lines
            {
              // Init Upscale
              int repeat_y = 0;
              uint16_t repeatTimes_y = 1;
              uint16_t upscaleCoef_y = floor(coef192 * (float)(y+1));
              repeatTimes_y = upscaleCoef_y - last_y;
              last_y = upscaleCoef_y;
              while (repeat_y < repeatTimes_y)
              {
                x_upscaled = margin_x;
                x = margin_x;
                last_x = margin_x;
                uint16_t lastPixel = 0;
                bool lastLine = false;
                bool lastCol = false;
                // End Upscale

                for (int c = 0; c < 0x28; c++)
                {
                  for (int k = 0; k < 7; k++)
                  {
                    // Init Upscale
                    bool bbPixel = 0;
                    if (smoothUpscale) {
                      if (c == 0x27 && k == 6)
                        lastCol = true;
                      if (repeat_y > 0) {
                        int ib = i;
                        int lb = l;
                        int bb = b;
                        ib++;
                        if (ib == 8) { ib=0; lb++; }
                        if (lb == 8) { lb=0; bb++; }
                        if (bb == 3) { bb=0; lastLine = true; }

                        char bottomChr = ram[(ushort)(textPage + (bb * 0x28) + (lb * 0x80) + c)];
                        
                        ushort bottomAddr = (bottomChr * 7 * 8) + (ib * 7) + k;
                        bbPixel = AppleIIe ? AppleIIeFontPixels[bottomAddr] : AppleFontPixels[bottomAddr];
                      }
                    }
                    // End Upscale
                    char chr = ram[(ushort)(textPage + (b * 0x28) + (l * 0x80) + c)];
                    ushort addr = (chr * 7 * 8) + (i * 7) + k;
                    bool bpixel = AppleIIe ? AppleIIeFontPixels[addr] : AppleFontPixels[addr];
                    bool inverted = false;
                    if (!AppleIIe)
                      inverted = chr >= 0x40 && chr < 0x80 && inversed;
                      tft.writeColor(bpixel ? (inverted ? TFT_BLACK : TFT_WHITE) : (inverted ? TFT_WHITE : TFT_BLACK), 1);
                    x++;
                  }
                }
                repeat_y++;
                y_upscaled++;
              }
              y++;
            }
          }
          else if (AppleIIe && !Cols40_80)
          {
            for (int i = 0; i < 8; i++)
            {
              // Init Upscale
              int repeat_y = 0;
              uint16_t upscaleCoef_y = floor(coef192 * (float)(y+1));
              uint16_t repeatTimes_y = upscaleCoef_y - last_y;
              last_y = upscaleCoef_y;
              while (repeat_y < repeatTimes_y)
              {
                x_upscaled = margin_x;
                x = margin_x;
                last_x = margin_x;
                uint16_t lastPixel = 0;
                bool lastLine = false;
                bool lastCol = false;
                char bottomChr;
                // End Upscale
                for (int j = 0; j < 0x50; j++)
                {
                  // Init Upscale
                  bool bbPixel = 0;
                  int ib = i;
                  if (smoothUpscale) {
                    if (j == 0x4f)
                      lastCol = true;
                    int lb = l;
                    int bb = b;
                    if (repeat_y > 0) {
                      ib++;
                      if (ib == 8) { ib=0; lb++; }
                      if (lb == 8) { lb=0; bb++; }
                      if (bb == 3) { bb=0; lastLine = true; }

                      if (j % 2 == 0)
                      {
                        bottomChr = auxram[0, (ushort)(0x400 + (bb * 0x28) + (lb * 0x80) + j / 2)];
                      }
                      else
                      {
                        bottomChr = ram[(ushort)(0x400 + (bb * 0x28) + (lb * 0x80) + (j - 1) / 2)];
                      }
                      
                    }
                  }
                  // End Upscale

                  char chr;
                  if (j % 2 == 0)
                  {
                    chr = auxram[0, (ushort)(0x400 + (b * 0x28) + (l * 0x80) + j / 2)];
                  }
                  else
                  {
                    chr = ram[(ushort)(0x400 + (b * 0x28) + (l * 0x80) + (j - 1) / 2)];
                  }
                  bool last7bits = false;
                  for (int k = 0; k < 7; k++)
                  {
                    ushort addr = (chr * 7 * 8) + (i * 7) + k;
                    bool bpixel = AppleIIeFontPixels[addr];
                    uint16_t color = 0;
                    if (k % 2 == 0)
                    {
                      if (bpixel && last7bits)
                        color = tft.color565(255, 255, 255);
                      else if (bpixel != last7bits)
                        color = tft.color565(127, 127, 127);
                      else
                        color = tft.color565(0, 0, 0);
                      tft.writeColor(color, 1);
                      x++;
                    }
                    last7bits = bpixel;
                  }
                }
                repeat_y++;
                y_upscaled++;
              }
              y++;
            }
          }
        }
      }
    }
    
    unsigned long endTime2 = millis();
    tft.endWrite();
    Vertical_blankingOn_Off = true;
    unsigned long endTime3 = millis();
    vTaskDelay(pdMS_TO_TICKS(5));
    unsigned long endTime4 = millis();
    tft.invertDisplay(true);
    flashCount++;
    if (flashCount > 7)
    {
      inversed = !inversed;
      flashCount = 0;
    }
    unsigned long endTime5 = millis();
    // Calculate and print the duration
    unsigned long duration1 = endTime1 - startTime;
    unsigned long duration2 = endTime2 - endTime1;
    unsigned long duration3 = endTime3 - endTime2;
    unsigned long duration4 = endTime4 - endTime3;
    unsigned long duration5 = endTime5 - endTime4;
    
    unsigned long duration = endTime5 - startTime;

  // Serial.printf("Execution time: %d %d %d %d %d total: %d\n", duration1, duration2, duration3, duration4, duration5, duration);

  }
}
