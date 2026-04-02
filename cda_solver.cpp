#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iostream>
#include <map>
#include <queue>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <atomic>
#include <unordered_map>
#include <vector>

#include <minisat/core/Solver.h>

using namespace Minisat;

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

struct Observation {
    std::map<std::string, bool> values;
};

struct CNF {
    int vars = 0;
    std::vector<std::vector<int>> clauses;

    int new_var() {
        ++vars;
        return vars;
    }

    void add_clause(std::initializer_list<int> c) {
        clauses.emplace_back(c);
    }

    void add_clause(const std::vector<int>& c) {
        clauses.push_back(c);
    }
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

static Observation parse_observation(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("Cannot open observation file: " + path);
    }

    Observation obs;
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

        size_t eq = line.find('=');
        if (eq == std::string::npos) {
            throw std::runtime_error("Observation line must be signal=value: " + line);
        }

        std::string name = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));
        if (val != "0" && val != "1") {
            throw std::runtime_error("Observation value must be 0/1: " + line);
        }
        obs.values[name] = (val == "1");
    }

    return obs;
}

static Lit dimacs_lit_to_minisat(int lit) {
    int v = std::abs(lit) - 1;
    bool sign = (lit < 0);
    return mkLit(v, sign);
}

class CDASolver {
public:
    CDASolver(Circuit c, Observation o, int timeout_seconds)
        : circuit_(std::move(c)), obs_(std::move(o)), timeout_seconds_(timeout_seconds) {
        for (int i = 0; i < static_cast<int>(circuit_.components.size()); ++i) {
            comp_index_[circuit_.components[i]] = i;
        }
        build_valid_signal_set();
        validate_observation_signals();
    }

    void run(bool first_only) {
        deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds_);

        struct Node {
            std::set<int> faulty;
            double g_cost;
            double h_cost;
            uint64_t order;
        };

        struct Cmp {
            bool operator()(const Node& a, const Node& b) const {
                double fa = a.g_cost + a.h_cost;
                double fb = b.g_cost + b.h_cost;
                if (fa != fb) return fa > fb;
                if (a.g_cost != b.g_cost) return a.g_cost > b.g_cost;
                return a.order > b.order;
            }
        };

        std::priority_queue<Node, std::vector<Node>, Cmp> open;
        uint64_t counter = 0;
        open.push(Node{{}, 0.0, 0.0, counter++});

        std::set<std::set<int>> closed;
        std::vector<std::set<int>> diagnoses;

        while (!open.empty()) {
            if (remaining_time_ms() <= 0) {
                throw std::runtime_error("Timeout");
            }

            Node cur = open.top();
            open.pop();
            if (closed.count(cur.faulty)) {
                continue;
            }
            closed.insert(cur.faulty);

            const std::set<int>* miss = first_unhit_conflict(cur.faulty);
            if (miss != nullptr) {
                for (int c : *miss) {
                    if (cur.faulty.count(c)) continue;
                    std::set<int> child = cur.faulty;
                    child.insert(c);
                    if (!closed.count(child)) {
                        open.push(Node{child, static_cast<double>(child.size()), 0.0, counter++});
                    }
                }
                continue;
            }

            if (is_diagnosis(cur.faulty)) {
                diagnoses.push_back(cur.faulty);
                if (first_only) {
                    print_diagnoses(diagnoses);
                    return;
                }
                continue;
            }

            std::set<int> healthy = complement_set(cur.faulty);
            std::set<int> conflict = minimize_conflict(healthy);
            if (conflict.empty()) {
                throw std::runtime_error("Extracted empty conflict unexpectedly");
            }
            add_conflict_if_new(conflict);

            for (int c : conflict) {
                if (cur.faulty.count(c)) continue;
                std::set<int> child = cur.faulty;
                child.insert(c);
                if (!closed.count(child)) {
                    open.push(Node{child, static_cast<double>(child.size()), 0.0, counter++});
                }
            }
        }

        print_diagnoses(diagnoses);
    }

private:
    Circuit circuit_;
    Observation obs_;
    int timeout_seconds_ = 30;
    std::chrono::steady_clock::time_point deadline_;

    std::map<std::string, int> comp_index_;
    std::set<std::string> valid_signals_;
    std::vector<std::set<int>> conflicts_;
    mutable std::unordered_map<std::string, bool> consistency_cache_;

    int remaining_time_ms() const {
        auto now = std::chrono::steady_clock::now();
        auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(deadline_ - now).count();
        return static_cast<int>(diff);
    }

    void build_valid_signal_set() {
        for (const auto& pi : circuit_.primary_inputs) {
            valid_signals_.insert(pi);
        }
        for (const auto& g : circuit_.gates) {
            valid_signals_.insert(g.output);
        }
    }

    void validate_observation_signals() const {
        std::vector<std::string> unknown;
        for (const auto& kv : obs_.values) {
            if (!valid_signals_.count(kv.first)) {
                unknown.push_back(kv.first);
            }
        }
        if (!unknown.empty()) {
            std::ostringstream oss;
            oss << "Observation contains signals not in this circuit: ";
            for (size_t i = 0; i < unknown.size(); ++i) {
                if (i) oss << ", ";
                oss << unknown[i];
            }
            throw std::runtime_error(oss.str());
        }
    }

    static std::string encode_set(const std::set<int>& s) {
        std::ostringstream oss;
        bool first = true;
        for (int v : s) {
            if (!first) oss << ',';
            first = false;
            oss << v;
        }
        return oss.str();
    }

    std::set<int> complement_set(const std::set<int>& s) const {
        std::set<int> out;
        for (int i = 0; i < static_cast<int>(circuit_.components.size()); ++i) {
            if (!s.count(i)) out.insert(i);
        }
        return out;
    }

    const std::set<int>* first_unhit_conflict(const std::set<int>& faulty) const {
        for (const auto& c : conflicts_) {
            bool hit = false;
            for (int x : c) {
                if (faulty.count(x)) {
                    hit = true;
                    break;
                }
            }
            if (!hit) {
                return &c;
            }
        }
        return nullptr;
    }

    void add_conflict_if_new(const std::set<int>& c) {
        for (const auto& old : conflicts_) {
            if (old == c) {
                return;
            }
        }
        conflicts_.push_back(c);
    }

    static void add_guarded_clause(CNF& cnf, int healthy_var, const std::vector<int>& lits) {
        std::vector<int> clause;
        clause.reserve(lits.size() + 1);
        clause.push_back(-healthy_var);
        clause.insert(clause.end(), lits.begin(), lits.end());
        cnf.add_clause(clause);
    }

    static void encode_gate(CNF& cnf, const std::string& type, const std::vector<int>& in, int out, int healthy) {
        if (type == "AND") {
            std::vector<int> c1;
            for (int li : in) c1.push_back(-li);
            c1.push_back(out);
            add_guarded_clause(cnf, healthy, c1);
            for (int li : in) add_guarded_clause(cnf, healthy, {li, -out});
            return;
        }
        if (type == "OR") {
            std::vector<int> c1;
            for (int li : in) c1.push_back(li);
            c1.push_back(-out);
            add_guarded_clause(cnf, healthy, c1);
            for (int li : in) add_guarded_clause(cnf, healthy, {-li, out});
            return;
        }
        if (type == "NAND") {
            std::vector<int> c1;
            for (int li : in) c1.push_back(-li);
            c1.push_back(-out);
            add_guarded_clause(cnf, healthy, c1);
            for (int li : in) add_guarded_clause(cnf, healthy, {li, out});
            return;
        }
        if (type == "NOR") {
            std::vector<int> c1;
            for (int li : in) c1.push_back(li);
            c1.push_back(out);
            add_guarded_clause(cnf, healthy, c1);
            for (int li : in) add_guarded_clause(cnf, healthy, {-li, -out});
            return;
        }
        if (type == "NOT") {
            if (in.size() != 1) throw std::runtime_error("NOT gate expects 1 input");
            add_guarded_clause(cnf, healthy, {in[0], out});
            add_guarded_clause(cnf, healthy, {-in[0], -out});
            return;
        }
        if (type == "BUF") {
            if (in.size() != 1) throw std::runtime_error("BUF gate expects 1 input");
            add_guarded_clause(cnf, healthy, {-in[0], out});
            add_guarded_clause(cnf, healthy, {in[0], -out});
            return;
        }
        if (type == "XOR") {
            if (in.size() != 2) throw std::runtime_error("XOR gate expects 2 inputs");
            int a = in[0], b = in[1];
            add_guarded_clause(cnf, healthy, {-a, -b, -out});
            add_guarded_clause(cnf, healthy, {a, b, -out});
            add_guarded_clause(cnf, healthy, {a, -b, out});
            add_guarded_clause(cnf, healthy, {-a, b, out});
            return;
        }
        if (type == "XNOR") {
            if (in.size() != 2) throw std::runtime_error("XNOR gate expects 2 inputs");
            int a = in[0], b = in[1];
            add_guarded_clause(cnf, healthy, {-a, -b, out});
            add_guarded_clause(cnf, healthy, {a, b, out});
            add_guarded_clause(cnf, healthy, {a, -b, -out});
            add_guarded_clause(cnf, healthy, {-a, b, -out});
            return;
        }
        throw std::runtime_error("Unsupported gate type: " + type);
    }

    static bool solve_cnf_with_minisat(const CNF& cnf, int timeout_ms) {
        Solver solver;
        for (int i = 0; i < cnf.vars; ++i) {
            solver.newVar();
        }

        for (const auto& cl : cnf.clauses) {
            vec<Lit> clause;
            for (int lit : cl) {
                clause.push(dimacs_lit_to_minisat(lit));
            }
            bool ok = solver.addClause_(clause);
            if (!ok) {
                return false;
            }
        }

        std::atomic<bool> finished{false};
        bool sat = false;

        std::thread worker([&]() {
            sat = solver.solve();
            finished.store(true, std::memory_order_release);
        });

        auto start = std::chrono::steady_clock::now();
        bool timeout = false;
        while (!finished.load(std::memory_order_acquire)) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
            if (elapsed >= timeout_ms) {
                timeout = true;
                solver.interrupt();
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        worker.join();
        if (timeout) {
            throw std::runtime_error("Timeout");
        }
        return sat;
    }

    bool check_assignment(const std::set<int>& faulty_set) const {
        int left_ms = remaining_time_ms();
        if (left_ms <= 0) {
            throw std::runtime_error("Timeout");
        }

        std::string key = encode_set(faulty_set);
        auto it = consistency_cache_.find(key);
        if (it != consistency_cache_.end()) {
            return it->second;
        }

        CNF cnf;
        std::map<std::string, int> signal_var;
        std::vector<int> health_var(circuit_.components.size(), 0);

        auto get_signal = [&](const std::string& name) -> int {
            auto it2 = signal_var.find(name);
            if (it2 != signal_var.end()) {
                return it2->second;
            }
            int v = cnf.new_var();
            signal_var[name] = v;
            return v;
        };

        for (int i = 0; i < static_cast<int>(circuit_.components.size()); ++i) {
            health_var[i] = cnf.new_var();
        }

        for (const auto& g : circuit_.gates) {
            int idx = comp_index_.at(g.output);
            int hv = health_var[idx];

            std::vector<int> in_lits;
            in_lits.reserve(g.inputs.size());
            for (const auto& in_name : g.inputs) {
                in_lits.push_back(get_signal(in_name));
            }
            int ov = get_signal(g.output);
            encode_gate(cnf, g.type, in_lits, ov, hv);
        }

        for (int i = 0; i < static_cast<int>(circuit_.components.size()); ++i) {
            bool healthy = (faulty_set.count(i) == 0);
            int hv = health_var[i];
            cnf.add_clause({healthy ? hv : -hv});
        }

        for (const auto& kv : obs_.values) {
            int sv = get_signal(kv.first);
            cnf.add_clause({kv.second ? sv : -sv});
        }

        bool sat = solve_cnf_with_minisat(cnf, left_ms);
        consistency_cache_[key] = sat;
        return sat;
    }

    bool is_diagnosis(const std::set<int>& faulty) const {
        return check_assignment(faulty);
    }

    bool is_conflict(const std::set<int>& healthy) const {
        std::set<int> faulty = complement_set(healthy);
        return !check_assignment(faulty);
    }

    std::set<int> minimize_conflict(std::set<int> cset) const {
        bool changed = true;
        while (changed) {
            changed = false;
            std::vector<int> elems(cset.begin(), cset.end());
            for (int x : elems) {
                std::set<int> cand = cset;
                cand.erase(x);
                if (is_conflict(cand)) {
                    cset.swap(cand);
                    changed = true;
                    break;
                }
            }
        }
        return cset;
    }

    void print_set_as_components(const std::set<int>& s) const {
        std::cout << "{";
        bool first = true;
        for (int i : s) {
            if (!first) std::cout << ", ";
            first = false;
            std::cout << circuit_.components[i];
        }
        std::cout << "}";
    }

    void print_diagnoses(const std::vector<std::set<int>>& diagnoses) const {
        if (diagnoses.empty()) {
            std::cout << "No diagnosis found.\n";
            return;
        }
        size_t best = static_cast<size_t>(-1);
        for (const auto& d : diagnoses) {
            best = std::min(best, d.size());
        }
        std::cout << "Diagnoses found: " << diagnoses.size() << "\n";
        std::cout << "Minimal cardinality = " << best << "\n";
        for (const auto& d : diagnoses) {
            if (d.size() == best) {
                std::cout << "  ";
                print_set_as_components(d);
                std::cout << "\n";
            }
        }
    }
};

static void usage(const char* prog) {
    std::cerr << "Usage: " << prog << " <bench_file> <observation_file> [--first] [--timeout <sec>]\n";
}

int main(int argc, char** argv) {
    if (argc < 3) {
        usage(argv[0]);
        return 1;
    }

    try {
        Circuit circuit = parse_bench(argv[1]);
        Observation obs = parse_observation(argv[2]);

        bool first_only = false;
        int timeout_seconds = 30;

        for (int i = 3; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--first") {
                first_only = true;
            } else if (arg == "--timeout") {
                if (i + 1 >= argc) {
                    throw std::runtime_error("Missing value after --timeout");
                }
                timeout_seconds = std::stoi(argv[++i]);
                if (timeout_seconds <= 0) {
                    throw std::runtime_error("--timeout must be > 0");
                }
            } else {
                throw std::runtime_error("Unknown argument: " + arg);
            }
        }

        CDASolver solver(std::move(circuit), std::move(obs), timeout_seconds);
        solver.run(first_only);
    } catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << "\n";
        return 2;
    }

    return 0;
}
