// Host unit tests for protocol.h
// Build + run:  sh run_tests.sh
#include <cstdio>
#include <cmath>
#include "protocol.h"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("  FAIL line %d: %s\n", __LINE__, #cond); failures++; } } while (0)

static void test_gps_round_trip() {
  printf("GPS packet encodes to the legacy 14-byte layout and decodes back\n");
  GpsReport in = { -45.123456, 168.654321, 3, 87, true };
  uint8_t buf[GPS_PACKET_LEN];
  CHECK(protoEncodeGps(buf, DEVICE_ID_IRRIGATOR, in) == 14);
  CHECK(buf[0] == 0x02 && buf[1] == 0x30);
  // -45123456 = 0xFD4F7880 big-endian
  CHECK(buf[2] == 0xFD && buf[3] == 0x4F && buf[4] == 0x78 && buf[5] == 0x80);
  CHECK(buf[12] == 87 && buf[13] == 1);

  GpsReport out;
  CHECK(protoDecodeGps(buf + 2, 12, out));
  CHECK(fabs(out.lat - in.lat) < 1e-6 && fabs(out.lon - in.lon) < 1e-6);
  CHECK(out.speedCms == 3 && out.battPct == 87 && out.pumpOn);
}

static void test_gps_decode_rejects_short() {
  printf("short GPS payload is rejected\n");
  uint8_t buf[12] = {0};
  GpsReport out;
  CHECK(!protoDecodeGps(buf, 11, out));
}

static void test_pos_packet() {
  printf("alert packet carries src, type and position\n");
  uint8_t buf[POS_PACKET_LEN];
  CHECK(protoEncodePos(buf, DEVICE_ID_PUMP, MSG_PUMP_CUTOFF, -45.5, 168.3) == 10);
  CHECK(buf[0] == 0x01 && buf[1] == 0x50);
  CHECK(protoGetI32(buf + 2) == -45500000);
  CHECK(protoGetI32(buf + 6) == 168300000);
}

int main() {
  test_gps_round_trip();
  test_gps_decode_rejects_short();
  test_pos_packet();
  printf(failures ? "\n%d FAILURE(S)\n" : "\nALL PASSED\n", failures);
  return failures ? 1 : 0;
}
