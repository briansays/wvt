// Device self-test (issue #11): UHD search results as device list entries
// (USB by serial, network by address), device argument parsing, and the
// sample rate rules for each driver. No hardware needed. Exit status is
// nonzero on failure.
#include <cstdio>
#include <string>

#include "sdr_device.h"

namespace {
int failures = 0;

void check(bool ok, const std::string& what) {
    printf("  [%s] %s\n", ok ? " ok " : "FAIL", what.c_str());
    if (!ok) ++failures;
}

void testArgs() {
    printf("device arguments\n");
    auto a = parseDeviceArgs("type=x300,addr=192.168.40.2,master_clock_rate=184.32e6");
    check(a.size() == 3 && a[0].first == "type" && a[1].second == "192.168.40.2" &&
              a[2].second == "184.32e6",
          "three key=value pairs, in order");
    a = parseDeviceArgs("serial=31A2B3C,,recv_frame_size");
    check(a.size() == 2 && a[1].first == "recv_frame_size" && a[1].second.empty(), "empty items skipped, bare key kept");
    check(parseDeviceArgs("").empty(), "empty args: no pairs");
}

void testFound() {
    printf("UHD search results\n");
    DeviceInfo d = uhdDeviceInfo("type=b200,name=MyB205,serial=31A2B3C,product=B205mini");
    check(d.driver == "uhd" && d.args == "serial=31A2B3C", "USB B205mini: selected by serial (" + d.args + ")");
    check(d.label == "USRP B205mini (serial 31A2B3C, MyB205)", "label: " + d.label);
    d = uhdDeviceInfo("type=x300,addr=192.168.40.2,fpga=HG,name=,serial=3150ABC,product=X310");
    check(d.args == "addr=192.168.40.2", "X310: selected by address (" + d.args + ")");
    check(d.label == "USRP X310 (192.168.40.2, serial 3150ABC)", "label: " + d.label);
    d = uhdDeviceInfo("type=n3xx,addr=192.168.20.2,claimed=False,mgmt_addr=192.168.1.10,product=n310,serial=31F9A1B");
    check(d.args == "addr=192.168.20.2" && d.label.find("n310") != std::string::npos, "N310 by address: " + d.label);
    d = uhdDeviceInfo("type=x4xx,addr=192.168.10.2,product=x410,serial=328AB12");
    check(d.args == "addr=192.168.10.2" && d.label.rfind("USRP x410", 0) == 0, "X410 by address: " + d.label);
    d = uhdDeviceInfo("type=usrp2,addr=192.168.10.3");
    check(d.args == "addr=192.168.10.3" && d.label == "USRP usrp2 (192.168.10.3)", "no product: the type names it");
    d = uhdDeviceInfo("type=b200");
    check(d.args == "type=b200" && d.label == "USRP b200", "no serial or address: the search string selects it");
}

void testRates() {
    printf("sample rates\n");
    std::string err, warn;
    check(checkSampleRate("uhd", 200e6, err, warn) && !warn.empty(), "USRP 200 MS/s allowed, with a link warning");
    check(checkSampleRate("uhd", 491.52e6, err, warn), "USRP 491.52 MS/s (X410) allowed");
    check(!checkSampleRate("uhd", 600e6, err, warn), "USRP 600 MS/s refused: " + err);
    check(checkSampleRate("uhd", 10e6, err, warn) && warn.empty(), "USRP 10 MS/s: no warning");
    check(!checkSampleRate("uhd", 0, err, warn), "zero refused");
    check(checkSampleRate("rtlsdr", 2.4e6, err, warn) && warn.empty(), "RTL-SDR 2.4 MS/s");
    check(!checkSampleRate("rtlsdr", 5e6, err, warn), "RTL-SDR 5 MS/s refused");
    check(!checkSampleRate("sim", 30e6, err, warn), "simulator above 20 MS/s refused");
}
}  // namespace

int main() {
    testArgs();
    testFound();
    testRates();
    printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
    return failures ? 1 : 0;
}
