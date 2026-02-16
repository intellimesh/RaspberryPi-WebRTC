#include "gnss/gnss_service.h"
#include "common/logging.h"
#include <iostream>
#include <istream>
#include <sstream>
#include <vector>

std::shared_ptr<GnssService> GnssService::Create(Args args, boost::asio::io_context &ioc) {
    return std::make_shared<GnssService>(args, ioc);
}

GnssService::GnssService(Args args, boost::asio::io_context &ioc) 
    : args_(args), serial_(ioc) {
    try {
        serial_.open(args.gnss_port);
        serial_.set_option(boost::asio::serial_port_base::baud_rate(115200));
        serial_.set_option(boost::asio::serial_port_base::character_size(8));
        serial_.set_option(boost::asio::serial_port_base::stop_bits(boost::asio::serial_port_base::stop_bits::one));
        serial_.set_option(boost::asio::serial_port_base::parity(boost::asio::serial_port_base::parity::none));
        serial_.set_option(boost::asio::serial_port_base::flow_control(boost::asio::serial_port_base::flow_control::none));
        
        INFO_PRINT("GnssService started on port: %s", args.gnss_port.c_str());
        Start();
    } catch (const std::exception &e) {
        ERROR_PRINT("Failed to open GNSS port %s: %s", args.gnss_port.c_str(), e.what());
    }
}

GnssService::~GnssService() {
    if (serial_.is_open()) {
        serial_.close();
    }
}

void GnssService::Start() {
    Read();
}

void GnssService::Read() {
    boost::asio::async_read_until(serial_, buffer_, '\n',
        [this](const boost::system::error_code &ec, std::size_t bytes_transferred) {
            if (!ec) {
                std::istream is(&buffer_);
                std::string line;
                std::getline(is, line);
                ParseNmea(line);
                Read();
            } else {
                ERROR_PRINT("Error reading from GNSS port: %s", ec.message().c_str());
                // Simple retry mechanism or backoff could be added
            }
        });
}

Location GnssService::GetLocation() {
    std::lock_guard<std::mutex> lock(location_mutex_);
    return current_location_;
}

// Simple NMEA split helper
std::vector<std::string> Split(const std::string &s, char delimiter) {
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream tokenStream(s);
    while (std::getline(tokenStream, token, delimiter)) {
        tokens.push_back(token);
    }
    return tokens;
}

void GnssService::ParseNmea(const std::string &line) {
    // Basic filtering for GPGGA or GPRMC
    // Example GPGGA: $GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47
    if (line.find("$GPGGA") != 0 && line.find("$GNGGA") != 0) {
        return;
    }

    auto tokens = Split(line, ',');
    if (tokens.size() < 6) return;

    // Check fix quality (index 6 in GPGGA)
    // 0 = invalid, 1 = GPS fix (SPS), 2 = DGPS fix
    if (tokens[6] == "0") {
         return; // No fix
    }

    try {
        std::string raw_lat = tokens[2];
        char lat_dir = tokens[3][0];
        std::string raw_lon = tokens[4];
        char lon_dir = tokens[5][0];

        if (raw_lat.empty() || raw_lon.empty()) return;

        double latitude = ConvertToDecimalDegrees(raw_lat, lat_dir);
        double longitude = ConvertToDecimalDegrees(raw_lon, lon_dir);

        {
            std::lock_guard<std::mutex> lock(location_mutex_);
            current_location_.latitude = latitude;
            current_location_.longitude = longitude;
            current_location_.has_fix = true;
        }
        
        // DEBUG_PRINT("GNSS Fix: Lat: %f, Lon: %f", latitude, longitude);

    } catch (...) {
        // Parsing error
    }
}

// Convert NMEA DDMM.MMMM to Decimal Degrees
double GnssService::ConvertToDecimalDegrees(const std::string &nmea_coord, char direction) {
    // Format: DDMM.MMMM
    // 4807.038 -> 48 deg 07.038 min
    
    // Find decimal point
    size_t decimal_pos = nmea_coord.find('.');
    if (decimal_pos == std::string::npos || decimal_pos < 2) return 0.0;
    
    // Degrees are everything before the last 2 digits of the integer part
    // e.g. 4807.038 -> 48 deg, 07.038 min -> split at index 2
    // e.g. 12345.67 -> 123 deg, 45.67 min -> split at index 3
    
    int split_idx = decimal_pos - 2;
    std::string deg_str = nmea_coord.substr(0, split_idx);
    std::string min_str = nmea_coord.substr(split_idx);
    
    double degrees = std::stod(deg_str);
    double minutes = std::stod(min_str);
    
    double decimal = degrees + (minutes / 60.0);
    
    if (direction == 'S' || direction == 'W') {
        decimal = -decimal;
    }
    
    return decimal;
}
