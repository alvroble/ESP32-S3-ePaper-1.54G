#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include "EPD_1in54g.h"
#include "GUI_Paint.h"
#include "fonts.h"
#include "ImageData.h"

#define UPDATE_INTERVAL_MS  (10UL * 60UL * 1000UL)

static const char* WIFI_SSID     = "BOOX24";
static const char* WIFI_PASSWORD = "Jw4iKrgbX9JB";

static const char* BTC_URL = "https://api.binance.com/api/v3/ticker/price?symbol=BTCUSDT";

static UBYTE *BlackImage;
static UWORD Imagesize;

static unsigned long lastUpdate = 0;
static float lastPrice = 0.0f;

static float fetchBTCPrice(void)
{
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    float price = NAN;

    http.setTimeout(10000);
    http.begin(client, BTC_URL);
    int httpCode = http.GET();

    if (httpCode == HTTP_CODE_OK) {
        String body = http.getString();
        int idx = body.indexOf("\"price\":\"");
        if (idx >= 0) {
            int start = idx + 9;
            int end   = body.indexOf("\"", start);
            if (end > start) {
                price = body.substring(start, end).toFloat();
            }
        }
    } else {
        printf("BTC fetch failed, HTTP %d\n", httpCode);
    }

    http.end();
    return price;
}

static int textWidth(const char *s, sFONT *font)
{
    int len = (int)strlen(s);
    return len * font->Width;
}

static void drawBtcScreen(float price, unsigned long ageSec)
{
    Paint_SelectImage(BlackImage);
    Paint_Clear(EPD_1IN54G_WHITE);

    Paint_DrawRectangle(0, 0, EPD_1IN54G_WIDTH - 1, 28, EPD_1IN54G_RED,
                        DOT_PIXEL_1X1, DRAW_FILL_FULL);
    const char *title = "BTC / USD";
    int titleW = textWidth(title, &Font16);
    Paint_DrawString_EN((EPD_1IN54G_WIDTH - titleW) / 2, 6, title, &Font16,
                        EPD_1IN54G_WHITE, EPD_1IN54G_RED);

    Paint_DrawString_EN(10, 34, "Bitcoin live price", &Font12,
                        EPD_1IN54G_BLACK, EPD_1IN54G_WHITE);

    char priceBuf[24];
    dtostrf(price, 0, 2, priceBuf);
    char fullPrice[28];
    snprintf(fullPrice, sizeof(fullPrice), "$%s", priceBuf);

    sFONT *priceFont = (strlen(fullPrice) > 11) ? &Font20 : &Font24;
    int priceW = textWidth(fullPrice, priceFont);
    int priceX = (EPD_1IN54G_WIDTH - priceW) / 2;
    Paint_DrawString_EN(priceX, 60, fullPrice, priceFont,
                        EPD_1IN54G_BLACK, EPD_1IN54G_WHITE);

    Paint_DrawLine(10, 110, EPD_1IN54G_WIDTH - 10, 110,
                   EPD_1IN54G_BLACK, DOT_PIXEL_1X1, LINE_STYLE_SOLID);

    char ageBuf[40];
    if (ageSec == 0) {
        snprintf(ageBuf, sizeof(ageBuf), "Just updated");
    } else {
        unsigned long min = ageSec / 60;
        unsigned long sec = ageSec % 60;
        snprintf(ageBuf, sizeof(ageBuf), "Updated %lu m %02lu s ago", min, sec);
    }
    Paint_DrawString_EN(10, 118, ageBuf, &Font12,
                        EPD_1IN54G_BLACK, EPD_1IN54G_WHITE);

    Paint_DrawRectangle(0, EPD_1IN54G_HEIGHT - 22, EPD_1IN54G_WIDTH - 1,
                        EPD_1IN54G_HEIGHT - 1, EPD_1IN54G_YELLOW,
                        DOT_PIXEL_1X1, DRAW_FILL_FULL);
    Paint_DrawString_EN(10, EPD_1IN54G_HEIGHT - 17, "Refresh every 10 min",
                        &Font12, EPD_1IN54G_BLACK, EPD_1IN54G_YELLOW);

    EPD_1IN54G_Init_Fast();
    EPD_1IN54G_Display(BlackImage);
}

static void drawErrorScreen(const char *msg)
{
    Paint_SelectImage(BlackImage);
    Paint_Clear(EPD_1IN54G_WHITE);

    Paint_DrawRectangle(0, 0, EPD_1IN54G_WIDTH - 1, 28, EPD_1IN54G_RED,
                        DOT_PIXEL_1X1, DRAW_FILL_FULL);
    Paint_DrawString_EN(10, 6, "BTC / USD", &Font16,
                        EPD_1IN54G_WHITE, EPD_1IN54G_RED);

    Paint_DrawString_EN(10, 70, "Fetch error", &Font24,
                        EPD_1IN54G_RED, EPD_1IN54G_WHITE);
    Paint_DrawString_EN(10, 110, msg, &Font12,
                        EPD_1IN54G_BLACK, EPD_1IN54G_WHITE);

    EPD_1IN54G_Init_Fast();
    EPD_1IN54G_Display(BlackImage);
}

static void connectWifi(void)
{
    printf("Connecting to %s", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        printf(".");
    }
    printf("\nWiFi connected, IP: %s\n", WiFi.localIP().toString().c_str());
}

void setup()
{
    printf("EPD_1IN54G BTC ticker\r\n");

    if (DEV_Module_Init() != 0) {
        DEV_Module_Exit();
        while (1);
    }

    printf("e-Paper Init and Clear...\r\n");
    EPD_1IN54G_Init();
    EPD_1IN54G_Clear(EPD_1IN54G_WHITE);
    DEV_Delay_ms(2000);

    Imagesize = ((EPD_1IN54G_WIDTH % 4 == 0) ? (EPD_1IN54G_WIDTH / 4)
                                             : (EPD_1IN54G_WIDTH / 4 + 1))
                * EPD_1IN54G_HEIGHT;
    BlackImage = (UBYTE *)malloc(Imagesize);
    if (BlackImage == NULL) {
        printf("malloc failed\r\n");
        while (1);
    }

    Paint_NewImage(BlackImage, EPD_1IN54G_WIDTH, EPD_1IN54G_HEIGHT, 0,
                   EPD_1IN54G_WHITE);
    Paint_SetScale(4);

    connectWifi();

    float price = fetchBTCPrice();
    lastUpdate = millis();
    if (!isnan(price) && price > 0.0f) {
        lastPrice = price;
        drawBtcScreen(price, 0);
    } else {
        drawErrorScreen("Check WiFi / API");
    }
}

void loop()
{
    unsigned long now = millis();
    if (now - lastUpdate >= UPDATE_INTERVAL_MS) {
        float price = fetchBTCPrice();
        lastUpdate = now;
        if (!isnan(price) && price > 0.0f) {
            lastPrice = price;
            drawBtcScreen(price, 0);
        } else {
            drawErrorScreen("Check WiFi / API");
        }
    }

    if (WiFi.status() != WL_CONNECTED) {
        WiFi.reconnect();
    }

    delay(1000);
}
