// https://github.com/gtortone/esp-xvcd (upstream)

#include <ESP8266WiFi.h>
#include "credentials.h"

// Pin out
static constexpr const int tms_gpio = D5;
static constexpr const int tck_gpio = D7;
static constexpr const int tdo_gpio = D4;
static constexpr const int tdi_gpio = D6;

//#define VERBOSE
#define MAX_WRITE_SIZE  512
#define ERROR_OK 1
// #define XVCD_AP_MODE
#define XVCD_STATIC_IP

IPAddress ip(192, 168, 1, 13);
IPAddress gateway(192, 168, 1, 1);
IPAddress netmask(255, 255, 255, 0);
const int port = 2542;

WiFiServer server(port);
WiFiClient client;

// JTAG buffers
uint8_t cmd[16];
uint8_t buffer[1024], result[512];

/* Transition delay coefficients */
static const unsigned int jtag_delay = 10;  // NOTE!

static std::uint32_t jtag_xfer(std::uint_fast8_t n, std::uint32_t tms, std::uint32_t tdi)
{
  std::uint32_t tdo = 0;
  for (uint_fast8_t i = 0; i < n; i++) {
    jtag_write(0, tms & 1, tdi & 1);
    tdo |= jtag_read() << i;
    jtag_write(1, tms & 1, tdi & 1);
    tms >>= 1;
    tdi >>= 1;
  }
  return tdo;
}

static bool jtag_read(void)
{
  return digitalRead(tdo_gpio) & 1;
}

static void jtag_write(std::uint_fast8_t tck, std::uint_fast8_t tms, std::uint_fast8_t tdi)
{
  digitalWrite(tck_gpio, tck);
  digitalWrite(tms_gpio, tms);
  digitalWrite(tdi_gpio, tdi);

  for (std::uint32_t i = 0; i < jtag_delay; i++)
    asm volatile ("nop");
}

static int jtag_init(void)
{
  pinMode(tdo_gpio, INPUT);
  pinMode(tdi_gpio, OUTPUT);
  pinMode(tck_gpio, OUTPUT);
  pinMode(tms_gpio, OUTPUT);

  digitalWrite(tdi_gpio, 0);
  digitalWrite(tck_gpio, 0);
  digitalWrite(tms_gpio, 1);

  return ERROR_OK;
}

int sread(void *target, int len) {

  uint8_t *t = (uint8_t *) target;

  while (len) {
    int r = client.read(t, len);
    if (r <= 0)
      return r;
    t += r;
    len -= r;
  }
  return 1;
}

int srcmd(void * target, int maxlen) {
  uint8_t *t = (uint8_t *) target;

  while (maxlen) {
    int r = client.read(t, 1);
    if (r <= 0)
      return r;

    if (*t == ':') {
      return 1;
    }

    t += r;
    maxlen -= r;
  }

  return 0;
}

void setup() {
  delay(1000);

  Serial.begin(115200);
  Serial.println();
  Serial.println();
  Serial.println();
#ifdef XVCD_AP_MODE
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(ip, ip, netmask);
  WiFi.softAP(MY_SSID, MY_PASSPHRASE);
#else
#ifdef XVCD_STATIC_IP
  WiFi.config(ip, gateway, netmask);
#endif
  WiFi.begin(MY_SSID, MY_PASSPHRASE);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("");
  Serial.println("WiFi connected");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());
#endif
  Serial.print("Starting XVC Server on port ");
  Serial.println(port);

  jtag_init();

  server.begin();
  server.setNoDelay(true);
}

void loop() {

start: if (!client.connected()) {
    // try to connect to a new client
    client = server.available();
  } else {
    // read data from the connected client
    if (client.available()) {
      while (client.connected()) {

        do {

          if (srcmd(cmd, 8) != 1)
            goto start;

          if (memcmp(cmd, "getinfo:", 8) == 0) {
#ifdef VERBOSE
            Serial.write("XVC_info\n");
#endif
            client.write("xvcServer_v1.0:");
            client.print(MAX_WRITE_SIZE);
            client.write("\n");
            goto start;
          }

          if (memcmp(cmd, "settck:", 7) == 0) {
#ifdef VERBOSE
            Serial.write("XVC_tck\n");
#endif
            int ntck;
            if (sread(&ntck, 4) != 1) {
              Serial.println("reading tck failed\n");
              goto start;
            }
            // Actually TCK frequency is fixed, but replying a fixed TCK will halt hw_server
            client.write((const uint8_t *)&ntck, 4);
            goto start;
          }

          if (memcmp(cmd, "shift:", 6) != 0) {
            cmd[15] = '\0';
            Serial.print("invalid cmd ");
            Serial.println((char *)cmd);
            goto start;
          }

          int len;
          if (sread(&len, 4) != 1) {
            Serial.println("reading length failed\n");
            goto start;
          }

          unsigned int nr_bytes = (len + 7) / 8;

#ifdef VERBOSE
          Serial.print("len = ");
          Serial.print(len);
          Serial.print(" nr_bytes = ");
          Serial.println(nr_bytes);
#endif
          if (nr_bytes * 2 > sizeof(buffer)) {
            Serial.println("buffer size exceeded");
            goto start;
          }

          if (sread(buffer, nr_bytes * 2) != 1) {
            Serial.println("reading data failed\n");
            goto start;
          }

          memset((uint8_t *)result, 0, nr_bytes);

          jtag_write(0, 1, 1);

          int bytesLeft = nr_bytes;
          int bitsLeft = len;
          int byteIndex = 0;
          uint32_t tdi, tms, tdo;

          while (bytesLeft > 0) {
            tms = 0;
            tdi = 0;
            tdo = 0;
            if (bytesLeft >= 4) {
              memcpy(&tms, &buffer[byteIndex], 4);
              memcpy(&tdi, &buffer[byteIndex + nr_bytes], 4);
              tdo = jtag_xfer(32, tms, tdi);
              memcpy(&result[byteIndex], &tdo, 4);
              bytesLeft -= 4;
              bitsLeft -= 32;
              byteIndex += 4;
            } else {
              memcpy(&tms, &buffer[byteIndex], bytesLeft);
              memcpy(&tdi, &buffer[byteIndex + nr_bytes], bytesLeft);
              tdo = jtag_xfer(bitsLeft, tms, tdi);
              memcpy(&result[byteIndex], &tdo, bytesLeft);
              bytesLeft = 0;
              break;
            }
          }

          jtag_write(0, 1, 0);

          if (client.write(result, nr_bytes) != nr_bytes) {
            Serial.println("write");
          }
        } while (1);
      }
    }
  }
}
