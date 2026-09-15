#include "sl_bus_departures.h"

#include <cassert>
#include <iostream>
#include <string>

using namespace sl_bus;

namespace {

const char* response_with_departures = R"json(
{
  "departures": [
    {
      "direction": "Sofia",
      "direction_code": 2,
      "destination": "Hjorthagen",
      "state": "EXPECTED",
      "display": "4 min",
      "line": {"designation": "57", "transport_mode": "BUS"}
    },
    {
      "direction": "Sofia",
      "direction_code": 1,
      "destination": "Hjorthagen",
      "display": "6 min",
      "line": {"designation": "57", "transport_mode": "BUS"}
    },
    {
      "direction": "Hjorthagen",
      "direction_code": 2,
      "destination": "Slussen",
      "display": "8 min",
      "line": {"designation": "57", "transport_mode": "BUS"}
    },
    {
      "direction": "Sofia",
      "direction_code": 2,
      "destination": "Hjorthagen",
      "display": "10 min",
      "line": {"designation": "16", "transport_mode": "BUS"}
    },
    {
      "direction": "Sofia",
      "direction_code": 2,
      "destination": "Hjorthagen",
      "display": "12 min",
      "line": {"designation": "57", "transport_mode": "TRAIN"}
    },
    {
      "direction": "Sofia",
      "direction_code": 2,
      "destination": "Hjorthagen",
      "display": "14 min",
      "line": {"designation": "57", "transport_mode": "BUS"},
      "deviations": "Short disruption"
    }
  ],
  "stop_deviations": [
    {"importance": 5, "consequence": "INFORMATION", "message": "Tullgårdsparken hållplats"},
    {"message": "Keep walking clear"}
  ]
}
)json";

const char* response_for_line_74 = R"json(
{
  "departures": [
    {
      "direction_code": 2,
      "destination": "Hornsberg",
      "state": "EXPECTED",
      "display": "2 min",
      "line": {"designation": "74", "transport_mode": "BUS"}
    },
    {
      "direction_code": 1,
      "destination": "Sickla udde",
      "state": "EXPECTED",
      "display": "5 min",
      "line": {"designation": "74", "transport_mode": "BUS"}
    },
    {
      "direction_code": 2,
      "destination": "Hornsberg",
      "state": "EXPECTED",
      "display": "8 min",
      "line": {"designation": "57", "transport_mode": "BUS"}
    },
    {
      "direction_code": 2,
      "destination": "Hornsberg",
      "state": "EXPECTED",
      "display": "11 min",
      "line": {"designation": "74", "transport_mode": "BUS"}
    }
  ]
}
)json";

}  // namespace

int main()
{
    const auto parsed = parse_departures(response_with_departures, "57", 2);
    assert(parsed.valid);
    assert(parsed.data.departure_count == 3);
    assert(parsed.data.departures[0].display == "4 min");
    assert(parsed.data.departures[0].destination == "Hjorthagen");
    assert(parsed.data.departures[1].display == "8 min");
    assert(parsed.data.departures[1].destination == "Slussen");
    assert(parsed.data.departures[2].display == "14 min");
    assert(parsed.data.departures[2].disruption == "Short disruption");
    assert(parsed.data.notice_count == 2);
    assert(parsed.data.notices[0].message == "Tullgårdsparken hållplats");

    const auto line_74 = parse_departures(response_for_line_74, "74", 2);
    assert(line_74.valid);
    assert(line_74.data.departure_count == 2);
    assert(line_74.data.departures[0].display == "2 min");
    assert(line_74.data.departures[0].destination == "Hornsberg");
    assert(line_74.data.departures[1].display == "11 min");

    const auto cancelled = parse_departures(R"json({
      "departures": [{
        "direction": "Sofia", "direction_code": "2", "destination": "Hjorthagen",
        "state": "CANCELLED", "display": "Inställd",
        "line": {"designation": "57", "transport_mode": "bus"}
      }]
    })json");
    assert(cancelled.valid);
    assert(cancelled.data.departure_count == 1);
    assert(cancelled.data.departures[0].cancelled);
    assert(cancelled.data.departures[0].display == "Inställd");
    assert(cancelled.data.departures[0].disruption == "Cancelled");

    const auto missing_fields = parse_departures(R"json({
      "departures": [
        {"direction_code": 2, "destination": "Hjorthagen", "display": "4 min"},
        {"direction_code": 2, "destination": "Hjorthagen", "display": "5 min",
         "line": {"designation": "57"}},
        {"direction_code": 2, "display": "6 min",
         "line": {"designation": "57", "transport_mode": "BUS"}}
      ]
    })json");
    assert(missing_fields.valid);
    assert(missing_fields.data.departure_count == 0);

    const auto empty = parse_departures(R"json({"departures": [], "stop_deviations": []})json");
    assert(empty.valid);
    assert(empty.data.departure_count == 0);

    assert(!parse_departures("not json").valid);
    assert(!parse_departures(R"json([])json").valid);
    assert(!parse_departures(R"json({"departures": {}})json").valid);
    assert(!parse_departures(std::string(kMaxResponseBytes + 1, 'x')).valid);

    const std::string utf8 = "Tullgårdsparken åäö";
    const auto clipped = bounded_text(utf8, 17);
    assert(clipped == "Tullgårdsparken ");
    assert(bounded_text("å", 2) == "å");
    assert(bounded_text("å", 1).empty());
    assert(parse_retry_after_seconds(" 600 ") == 600);
    assert(parse_retry_after_seconds("tomorrow") == 0);
    assert(retry_delay_seconds(1) == 60);
    assert(retry_delay_seconds(2) == 120);
    assert(retry_delay_seconds(3) == 300);
    assert(retry_delay_seconds(1, 600) == 600);

    DepartureStore store;
    store.publish_success(0, parsed.data, 1000);
    auto snapshot = store.snapshot(90'999);
    assert(!snapshot.stops[0].expired);
    assert(snapshot.stops[0].fetch_status == FetchStatus::Success);
    store.publish_failure(0, "request failed");
    snapshot = store.snapshot(90'999);
    assert(!snapshot.stops[0].expired);
    assert(snapshot.stops[0].departure_count == 3);
    assert(snapshot.stops[0].fetch_status == FetchStatus::Failed);
    snapshot = store.snapshot(91'000);
    assert(snapshot.stops[0].expired);
    assert(snapshot.stops[0].departure_count == 3);
    store.publish_success(1, empty.data, 200'000);
    snapshot = store.snapshot(200'001);
    assert(!snapshot.stops[1].expired);
    assert(snapshot.stops[1].fetch_status == FetchStatus::Empty);
    assert(snapshot.stops[0].departure_count == 3);

    StopDepartureData untrusted;
    untrusted.departure_count = 99;
    untrusted.departures[0].display.assign(500, 'x');
    untrusted.notice_count = 99;
    untrusted.notices[0].message.assign(500, 'x');
    store.publish_success(1, untrusted, 300'000);
    snapshot = store.snapshot(300'001);
    assert(snapshot.stops[1].departure_count == kMaxDepartures);
    assert(snapshot.stops[1].notice_count == kMaxNotices);
    assert(snapshot.stops[1].departures[0].display.size() <= kMaxTextBytes);
    assert(snapshot.stops[1].notices[0].message.size() <= kMaxTextBytes);

    std::cout << "PASS: SL filtering, direction codes, bounded text, notices, failures and freshness\n";
}
