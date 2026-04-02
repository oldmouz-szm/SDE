#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

struct Gate {
    std::string output;
    std::string type;
    std::vector<std::string> inputs;
};

struct Circuit {
    std::vector<std::string> primary_inputs;
    std::vector<std::string> primary_outputs;
    std::vector<Gate> gates;
    std::vector<std::string> components;
};

static std::string trim(const std::string& s) {
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) {
        ++b;
    }
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) {
        --e;
    }
    return s.substr(b, e - b);
}

static std::vector<std::string> split_csv(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char ch : s) {
        if (ch == ',') {
            out.push_back(trim(cur));
            cur.clear();
        } else {
            cur.push_back(ch);
        }
    }
    if (!cur.empty()) {
        out.push_back(trim(cur));
    }
    return out;
}

static Circuit parse_bench(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("Cannot open bench file: " + path);
    }

    Circuit c;
    std::string line;
    while (std::getline(in, line)) {
        size_t hash = line.find('#');
        if (hash != std::string::npos) {
            line = line.substr(0, hash);
        }
        line = trim(line);
        if (line.empty()) {
            continue;
        }

        if (line.rfind("INPUT(", 0) == 0) {
            size_t l = line.find('(');
            size_t r = line.find(')');
            c.primary_inputs.push_back(trim(line.substr(l + 1, r - l - 1)));
            continue;
        }
        if (line.rfind("OUTPUT(", 0) == 0) {
            size_t l = line.find('(');
            size_t r = line.find(')');
            c.primary_outputs.push_back(trim(line.substr(l + 1, r - l - 1)));
            continue;
        }

        size_t eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }

        Gate g;
        g.output = trim(line.substr(0, eq));
        std::string rhs = trim(line.substr(eq + 1));
        size_t l = rhs.find('(');
        size_t r = rhs.rfind(')');
        if (l == std::string::npos || r == std::string::npos || r <= l) {
            throw std::runtime_error("Malformed gate line: " + line);
        }

        g.type = trim(rhs.substr(0, l));
        if (g.type == "BUFF") {
            g.type = "BUF";
        }
        g.inputs = split_csv(rhs.substr(l + 1, r - l - 1));

        c.gates.push_back(g);
        c.components.push_back(g.output);
    }

    return c;
}

static bool eval_gate(const Gate& g, const std::map<std::string, int>& v) {
    auto get = [&](const std::string& n) -> int {
        auto it = v.find(n);
        if (it == v.end()) {
            throw std::runtime_error("Signal not assigned yet: " + n);
        }
        return it->second;
    };

    if (g.type == "NOT") {
        return !get(g.inputs[0]);
    }
    if (g.type == "BUF") {
        return get(g.inputs[0]);
    }
    if (g.type == "AND") {
        int x = 1;
        for (const auto& in : g.inputs) {
            x &= get(in);
        }
        return x;
    }
    if (g.type == "OR") {
        int x = 0;
        for (const auto& in : g.inputs) {
            x |= get(in);
        }
        return x;
    }
    if (g.type == "NAND") {
        int x = 1;
        for (const auto& in : g.inputs) {
            x &= get(in);
        }
        return !x;
    }
    if (g.type == "NOR") {
        int x = 0;
        for (const auto& in : g.inputs) {
            x |= get(in);
        }
        return !x;
    }
    if (g.type == "XOR") {
        if (g.inputs.size() != 2) {
            throw std::runtime_error("XOR expects 2 inputs");
        }
        return get(g.inputs[0]) ^ get(g.inputs[1]);
    }
    if (g.type == "XNOR") {
        if (g.inputs.size() != 2) {
            throw std::runtime_error("XNOR expects 2 inputs");
        }
        return !(get(g.inputs[0]) ^ get(g.inputs[1]));
    }
    throw std::runtime_error("Unsupported gate type: " + g.type);
}

static std::map<std::string, int> simulate(const Circuit& c,
                                           const std::map<std::string, int>& pi_values,
                                           const std::set<std::string>& faulty_components,
                                           std::mt19937& rng) {
    std::map<std::string, int> values;
    for (const auto& pi : c.primary_inputs) {
        auto it = pi_values.find(pi);
        if (it == pi_values.end()) {
            throw std::runtime_error("Missing PI assignment: " + pi);
        }
        values[pi] = it->second;
    }

    std::uniform_int_distribution<int> bit(0, 1);

    // ISCAS85 bench order is typically topological, and this also naturally supports propagation.
    for (const auto& g : c.gates) {
        int out = eval_gate(g, values);
        if (faulty_components.count(g.output)) {
            out = bit(rng);
        }
        values[g.output] = out;
    }

    return values;
}

static std::string basename_no_ext(const std::string& p) {
    std::filesystem::path fp(p);
    return fp.stem().string();
}

static void usage(const char* prog) {
    std::cerr << "Usage: " << prog << " <bench_file> <out_dir> <num_cases> <fixed_faults> [seed]\n";
    std::cerr << "Example: " << prog << " ../iscas85/bench/c17.bench generated_obs/c17 50 2 123\n";
}

int main(int argc, char** argv) {
    if (argc < 5) {
        usage(argv[0]);
        return 1;
    }

    try {
        std::string bench_file = argv[1];
        std::string out_dir = argv[2];
        int num_cases = std::stoi(argv[3]);
        int fixed_faults = std::stoi(argv[4]);
        unsigned seed = (argc >= 6) ? static_cast<unsigned>(std::stoul(argv[5])) : std::random_device{}();

        if (num_cases <= 0) {
            throw std::runtime_error("num_cases must be > 0");
        }
        if (fixed_faults <= 0) {
            throw std::runtime_error("fixed_faults must be > 0");
        }

        Circuit c = parse_bench(bench_file);
        if (c.components.empty()) {
            throw std::runtime_error("No components parsed from bench");
        }
        if (fixed_faults > static_cast<int>(c.components.size())) {
            throw std::runtime_error("fixed_faults exceeds component count");
        }

        std::filesystem::create_directories(out_dir);

        std::mt19937 rng(seed);
        std::uniform_int_distribution<int> bit(0, 1);

        std::string base = basename_no_ext(bench_file);

        for (int i = 0; i < num_cases; ++i) {
            std::map<std::string, int> pi_values;
            std::set<std::string> faulty;
            std::map<std::string, int> values;

            bool accepted = false;
            const int max_attempts = 200;
            for (int attempt = 0; attempt < max_attempts; ++attempt) {
                pi_values.clear();
                for (const auto& pi : c.primary_inputs) {
                    pi_values[pi] = bit(rng);
                }

                int k = fixed_faults;
                std::vector<std::string> shuffled = c.components;
                std::shuffle(shuffled.begin(), shuffled.end(), rng);
                faulty.clear();
                for (int j = 0; j < k; ++j) {
                    faulty.insert(shuffled[j]);
                }

                auto healthy_values = simulate(c, pi_values, {}, rng);
                values = simulate(c, pi_values, faulty, rng);

                bool po_changed = false;
                for (const auto& po : c.primary_outputs) {
                    if (healthy_values[po] != values[po]) {
                        po_changed = true;
                        break;
                    }
                }

                if (po_changed) {
                    accepted = true;
                    break;
                }
            }

            if (!accepted) {
                throw std::runtime_error("Failed to generate observable faulty sample after many attempts");
            }

            std::ostringstream fn;
            fn << out_dir << "/" << base << "_obs_" << i << ".txt";
            std::ofstream out(fn.str());
            if (!out) {
                throw std::runtime_error("Cannot write observation file: " + fn.str());
            }

            out << "# Auto-generated observation\n";
            out << "# faults=";
            bool first = true;
            for (const auto& f : faulty) {
                if (!first) out << ",";
                first = false;
                out << f;
            }
            out << "\n";

            for (const auto& pi : c.primary_inputs) {
                out << pi << "=" << values[pi] << "\n";
            }
            for (const auto& po : c.primary_outputs) {
                out << po << "=" << values[po] << "\n";
            }
        }

        std::cout << "Generated " << num_cases << " observation files in: " << out_dir << "\n";
        std::cout << "Seed: " << seed << "\n";
    } catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << "\n";
        return 2;
    }

    return 0;
}
