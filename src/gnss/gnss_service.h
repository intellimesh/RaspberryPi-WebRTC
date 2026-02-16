#ifndef GNSS_SERVICE_H
#define GNSS_SERVICE_H

#include "args.h"
#include <atomic>
#include <boost/asio.hpp>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

struct Location {
    double latitude = 0.0;
    double longitude = 0.0;
    bool has_fix = false;
};

class GnssService {
  public:
    static std::shared_ptr<GnssService> Create(Args args, boost::asio::io_context &ioc);

    GnssService(Args args, boost::asio::io_context &ioc);
    ~GnssService();

    void Start();
    Location GetLocation();

  private:
    void Read();
    void ParseNmea(const std::string &line);
    double ConvertToDecimalDegrees(const std::string &nmea_coord, char direction);

    Args args_;
    boost::asio::serial_port serial_;
    boost::asio::streambuf buffer_;
    
    std::mutex location_mutex_;
    Location current_location_;
};

#endif // GNSS_SERVICE_H
