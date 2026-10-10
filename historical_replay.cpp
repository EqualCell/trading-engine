#include "historical_market.h"

#include <iostream>

int main(int argc, char* argv[]) {
    try {
        if (argc != 9 || std::string(argv[1]) != "--snapshots" || std::string(argv[3]) != "--trades" ||
            std::string(argv[5]) != "--provenance" || std::string(argv[7]) != "--output" ||
            !argv[2][0] || !argv[4][0] || !argv[6][0] || !argv[8][0]) {
            std::cerr << "Usage: " << argv[0] << " --snapshots FILE --trades FILE --provenance FILE --output FILE\n";
            return 1;
        }
        if (std::filesystem::exists(argv[8])) {
            for (int i : {2, 4, 6}) historical::require(!std::filesystem::equivalent(argv[i], argv[8]), "Output aliases an input file");
        }
        const auto session = historical::readSession(argv[2], argv[4], argv[6]);
        for (const auto& anomaly : session.anomalies) std::cerr << "Data anomaly: " << anomaly << '\n';
        std::ofstream out(argv[8], std::ios::binary);
        if (!out) throw std::runtime_error("Cannot open historical output");
        historical::writeSession(out, session);
        if (!out) throw std::runtime_error("Error writing historical output");
        out.flush();
        if (!out) throw std::runtime_error("Error flushing historical output");
        out.close();
        if (!out) throw std::runtime_error("Error closing historical output");
        std::cout << "Exported " << session.snapshots.size() << " snapshots and " << session.trades.size() << " recorded trades\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Historical replay failed: " << error.what() << '\n';
        return 1;
    }
}
