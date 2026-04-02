#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <queue>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <random>
#include <chrono>
#include <thread>
#include <atomic>
#include <unordered_set>
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

static Lit dimacs_lit_to_minisat(int lit) {
    int v = std::abs(lit) - 1;
    bool sign = (lit < 0);
    return mkLit(v, sign);
}

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
        std::string args = rhs.substr(l + 1, r - l - 1);
        g.inputs = split_csv(args);

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

class DiagnosisEngine {
public:
    DiagnosisEngine(Circuit circuit, Observation observation, int timeout_seconds)
        : c_(std::move(circuit)), obs_(std::move(observation)), timeout_seconds_(timeout_seconds) {
        for (int i = 0; i < static_cast<int>(c_.components.size()); ++i) {
            comp_index_[c_.components[i]] = i;
        }
        build_valid_signal_set();
        validate_observation_signals();
    }

    void run_sde(bool stop_after_first = false) {
        deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds_);

        const int n = static_cast<int>(c_.components.size());
        std::set<int> all_components;
        for (int i = 0; i < n; ++i) {
            all_components.insert(i);
        }

        if (is_conflict(all_components)) {
            std::set<int> cmin = minimize_conflict(all_components);
            conflicts_.push_back(cmin);
        } else {
            std::cout << "All-healthy assignment is consistent. Empty diagnosis is valid.\n";
            diagnoses_.insert(std::set<int>{});
            print_diagnoses();
            return;
        }

        if (!stop_after_first && is_diagnosis(all_components)) {
            std::set<int> dmin = minimize_diagnosis(all_components);
            diagnoses_.insert(dmin);
        }

        int iterations = 0;
        while (true) {
            ++iterations;
            pdds_step_conflict();
            StepResult diag_step = pdds_step_diagnosis();

            // Align with paper: for minimal-cardinality-first mode, stop on diagnosis-search PDDS outcome.
            if (stop_after_first && diag_step.new_diagnosis) {
                break;
            }
            if (diag_step.done) {
                break;
            }
            if (iterations > 100000) {
                std::cerr << "Reached iteration limit; stopping.\n";
                break;
            }
        }

        print_diagnoses();
    }

private:
    struct StepResult {
        bool done = false;
        bool new_diagnosis = false;
    };

    Circuit c_;
    Observation obs_;
    std::map<std::string, int> comp_index_;
    std::set<std::string> valid_signals_;
    std::mt19937 rng_{std::random_device{}()};
    int timeout_seconds_ = 30;
    std::chrono::steady_clock::time_point deadline_;

    std::vector<std::set<int>> conflicts_;
    std::set<std::set<int>> diagnoses_;
    std::set<std::set<int>> tried_diag_hs_;
    std::set<std::set<int>> tried_conf_hs_;
    mutable std::unordered_map<std::string, bool> consistency_cache_;

    void build_valid_signal_set() {
        for (const auto& pi : c_.primary_inputs) {
            valid_signals_.insert(pi);
        }
        for (const auto& g : c_.gates) {
            valid_signals_.insert(g.output);
        }
    }

    int remaining_time_ms() const {
        auto now = std::chrono::steady_clock::now();
        auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(deadline_ - now).count();
        return static_cast<int>(diff);
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
                if (i != 0) {
                    oss << ", ";
                }
                oss << unknown[i];
            }
            throw std::runtime_error(oss.str());
        }
    }

    static std::string encode_set(const std::set<int>& s) {
        std::ostringstream oss;
        bool first = true;
        for (int v : s) {
            if (!first) {
                oss << ',';
            }
            first = false;
            oss << v;
        }
        return oss.str();
    }

    static bool hits_all(const std::set<int>& candidate, const std::vector<std::set<int>>& collection) {
        for (const auto& t : collection) {
            bool hit = false;
            for (int e : t) {
                if (candidate.count(e)) {
                    hit = true;
                    break;
                }
            }
            if (!hit) {
                return false;
            }
        }
        return true;
    }

    static std::set<int> first_unhit_set(const std::set<int>& candidate, const std::vector<std::set<int>>& collection) {
        for (const auto& t : collection) {
            bool hit = false;
            for (int e : t) {
                if (candidate.count(e)) {
                    hit = true;
                    break;
                }
            }
            if (!hit) {
                return t;
            }
        }
        return {};
    }

    std::set<int> next_hitting_set(const std::vector<std::set<int>>& collection,
                                   const std::set<std::set<int>>& avoid,
                                   std::set<std::set<int>>& tried) {
        if (collection.empty()) {
            return {};
        }

        std::queue<std::set<int>> q;
        std::unordered_set<std::string> visited;
        q.push({});
        visited.insert(encode_set({}));

        while (!q.empty()) {
            std::set<int> cur = q.front();
            q.pop();

            if (avoid.count(cur) || tried.count(cur)) {
                // skip
            } else if (hits_all(cur, collection)) {
                tried.insert(cur);
                return cur;
            }

            std::set<int> unhit = first_unhit_set(cur, collection);
            if (unhit.empty()) {
                continue;
            }

            for (int e : unhit) {
                if (cur.count(e)) {
                    continue;
                }
                std::set<int> nxt = cur;
                nxt.insert(e);
                std::string key = encode_set(nxt);
                if (!visited.count(key)) {
                    visited.insert(key);
                    q.push(nxt);
                }
            }
        }

        return {};
    }

    std::set<int> complement_set(const std::set<int>& s) const {
        std::set<int> out;
        for (int i = 0; i < static_cast<int>(c_.components.size()); ++i) {
            if (!s.count(i)) {
                out.insert(i);
            }
        }
        return out;
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
            if (in.size() != 1) {
                throw std::runtime_error("NOT gate expects 1 input");
            }
            add_guarded_clause(cnf, healthy, {in[0], out});
            add_guarded_clause(cnf, healthy, {-in[0], -out});
            return;
        }
        if (type == "BUF") {
            if (in.size() != 1) {
                throw std::runtime_error("BUF gate expects 1 input");
            }
            add_guarded_clause(cnf, healthy, {-in[0], out});
            add_guarded_clause(cnf, healthy, {in[0], -out});
            return;
        }
        if (type == "XOR") {
            if (in.size() != 2) {
                throw std::runtime_error("XOR gate expects 2 inputs");
            }
            int a = in[0], b = in[1];
            add_guarded_clause(cnf, healthy, {-a, -b, -out});
            add_guarded_clause(cnf, healthy, {a, b, -out});
            add_guarded_clause(cnf, healthy, {a, -b, out});
            add_guarded_clause(cnf, healthy, {-a, b, out});
            return;
        }
        if (type == "XNOR") {
            if (in.size() != 2) {
                throw std::runtime_error("XNOR gate expects 2 inputs");
            }
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
        auto cache_it = consistency_cache_.find(key);
        if (cache_it != consistency_cache_.end()) {
            return cache_it->second;
        }

        CNF cnf;
        std::map<std::string, int> signal_var;
        std::vector<int> health_var(c_.components.size(), 0);

        auto get_signal = [&](const std::string& name) -> int {
            auto it = signal_var.find(name);
            if (it != signal_var.end()) {
                return it->second;
            }
            int v = cnf.new_var();
            signal_var[name] = v;
            return v;
        };

        for (int i = 0; i < static_cast<int>(c_.components.size()); ++i) {
            health_var[i] = cnf.new_var();
        }

        for (const auto& g : c_.gates) {
            auto it = comp_index_.find(g.output);
            if (it == comp_index_.end()) {
                throw std::runtime_error("Gate output is not a component: " + g.output);
            }
            int idx = it->second;
            int hv = health_var[idx];

            std::vector<int> in_lits;
            in_lits.reserve(g.inputs.size());
            for (const auto& in_name : g.inputs) {
                in_lits.push_back(get_signal(in_name));
            }
            int ov = get_signal(g.output);
            encode_gate(cnf, g.type, in_lits, ov, hv);
        }

        for (int i = 0; i < static_cast<int>(c_.components.size()); ++i) {
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

    bool is_diagnosis(const std::set<int>& diagnosis_faulty) const {
        return check_assignment(diagnosis_faulty);
    }

    bool is_conflict(const std::set<int>& conflict_healthy) const {
        std::set<int> faulty = complement_set(conflict_healthy);
        return !check_assignment(faulty);
    }

    std::set<int> minimize_diagnosis(std::set<int> d) {
        bool changed = true;
        while (changed) {
            changed = false;
            std::vector<int> elems(d.begin(), d.end());
            std::shuffle(elems.begin(), elems.end(), rng_);
            for (int x : elems) {
                std::set<int> cand = d;
                cand.erase(x);
                if (is_diagnosis(cand)) {
                    d.swap(cand);
                    changed = true;
                    break;
                }
            }
        }
        return d;
    }

    std::set<int> minimize_conflict(std::set<int> cset) {
        bool changed = true;
        while (changed) {
            changed = false;
            std::vector<int> elems(cset.begin(), cset.end());
            std::shuffle(elems.begin(), elems.end(), rng_);
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

    StepResult pdds_step_diagnosis() {
        StepResult out;
        std::set<int> w = next_hitting_set(conflicts_, diagnoses_, tried_diag_hs_);
        if (w.empty() && !conflicts_.empty()) {
            out.done = true;
            return out;
        }

        if (is_diagnosis(w)) {
            std::set<int> dmin = minimize_diagnosis(w);
            auto inserted = diagnoses_.insert(dmin);
            out.new_diagnosis = inserted.second;
        } else {
            std::set<int> conf = complement_set(w);
            conflicts_.push_back(minimize_conflict(conf));
        }
        return out;
    }

    void pdds_step_conflict() {
        std::vector<std::set<int>> diag_collection(diagnoses_.begin(), diagnoses_.end());
        if (diag_collection.empty()) {
            return;
        }

        std::set<int> w = next_hitting_set(diag_collection, {}, tried_conf_hs_);
        if (w.empty()) {
            return;
        }

        if (is_conflict(w)) {
            conflicts_.push_back(minimize_conflict(w));
        } else {
            std::set<int> d = complement_set(w);
            diagnoses_.insert(minimize_diagnosis(d));
        }
    }

    void print_set_as_components(const std::set<int>& s) const {
        std::cout << "{";
        bool first = true;
        for (int i : s) {
            if (!first) {
                std::cout << ", ";
            }
            first = false;
            std::cout << c_.components[i];
        }
        std::cout << "}";
    }

    void print_diagnoses() const {
        if (diagnoses_.empty()) {
            std::cout << "No diagnosis found.\n";
            return;
        }

        std::cout << "Diagnoses found: " << diagnoses_.size() << "\n";
        size_t best = static_cast<size_t>(-1);
        for (const auto& d : diagnoses_) {
            best = std::min(best, d.size());
        }

        std::cout << "Minimal cardinality = " << best << "\n";
        for (const auto& d : diagnoses_) {
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
    std::cerr << "Observation format: one signal=value per line, e.g. a=1\n";
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

        DiagnosisEngine engine(std::move(circuit), std::move(obs), timeout_seconds);
        engine.run_sde(first_only);
    } catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << "\n";
        return 2;
    }

    return 0;
}
