// Extensible Workflow Automation Engine - prototype (C++17)
// Build: g++ -std=c++17 -Wall -o workflow workflow_engine.cpp && ./workflow
//
// Design patterns used:
//   Command    : Task interface (each step is an executable object)
//   Factory    : TaskFactory creates tasks from a string type name
//   Singleton  : TaskFactory has exactly one registry
//   Registry   : tasks self-register -> engine never changes when you add tasks
//   Decorator  : RetryTask wraps any Task and adds retry behaviour
//   Observer   : Workflow notifies listeners (e.g. logger) about events
//   Builder    : WorkflowBuilder gives a fluent way to assemble a workflow

#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using Params = std::map<std::string, std::string>;

// Shared data passed between tasks (like variables in the workflow)
struct Context {
    std::map<std::string, std::string> vars;
};

// Replace ${name} in text with values from the context
std::string expand(const std::string& text, const Context& ctx) {
    std::string out;
    for (size_t i = 0; i < text.size();) {
        if (text.compare(i, 2, "${") == 0) {
            size_t end = text.find('}', i);
            if (end != std::string::npos) {
                auto it = ctx.vars.find(text.substr(i + 2, end - i - 2));
                out += (it != ctx.vars.end()) ? it->second : "";
                i = end + 1;
                continue;
            }
        }
        out += text[i++];
    }
    return out;
}

// ---------------------------------------------------------------- Command
class Task {
public:
    virtual ~Task() = default;
    virtual std::string name() const = 0;
    virtual bool execute(Context& ctx) = 0;  // true = success
};

// ---------------------------------------------------------------- Singleton + Factory
class TaskFactory {
public:
    using Creator = std::function<std::unique_ptr<Task>(const Params&)>;

    static TaskFactory& instance() {
        static TaskFactory f;
        return f;
    }
    void registerTask(const std::string& type, Creator c) { creators_[type] = std::move(c); }

    std::unique_ptr<Task> create(const std::string& type, const Params& p) const {
        auto it = creators_.find(type);
        if (it == creators_.end()) throw std::runtime_error("Unknown task type: " + type);
        return it->second(p);
    }

private:
    TaskFactory() = default;
    std::map<std::string, Creator> creators_;
};

// Helper: a global object of this type registers a task at program start
struct Registrar {
    Registrar(const std::string& type, TaskFactory::Creator c) {
        TaskFactory::instance().registerTask(type, std::move(c));
    }
};

// ---------------------------------------------------------------- Concrete tasks
class PrintTask : public Task {
public:
    explicit PrintTask(Params p) : p_(std::move(p)) {}
    std::string name() const override { return "print"; }
    bool execute(Context& ctx) override {
        std::cout << "    [print] " << expand(p_["message"], ctx) << "\n";
        return true;
    }
private:
    Params p_;
};

class SetVarTask : public Task {
public:
    explicit SetVarTask(Params p) : p_(std::move(p)) {}
    std::string name() const override { return "set_var"; }
    bool execute(Context& ctx) override {
        ctx.vars[p_["key"]] = expand(p_["value"], ctx);
        return true;
    }
private:
    Params p_;
};

// Fails the first N attempts - used to demonstrate the retry decorator
class FlakyTask : public Task {
public:
    explicit FlakyTask(Params p) : failures_(std::stoi(p["fail_times"])) {}
    std::string name() const override { return "flaky"; }
    bool execute(Context&) override {
        if (failures_-- > 0) { std::cout << "    [flaky] failed\n"; return false; }
        std::cout << "    [flaky] succeeded\n";
        return true;
    }
private:
    int failures_;
};

// To add a new task: write a class + one Registrar line. Nothing else changes.
namespace {
Registrar r1("print",   [](const Params& p) { return std::make_unique<PrintTask>(p); });
Registrar r2("set_var", [](const Params& p) { return std::make_unique<SetVarTask>(p); });
Registrar r3("flaky",   [](const Params& p) { return std::make_unique<FlakyTask>(p); });
}

// ---------------------------------------------------------------- Decorator
class RetryTask : public Task {
public:
    RetryTask(std::unique_ptr<Task> inner, int attempts)
        : inner_(std::move(inner)), attempts_(attempts) {}
    std::string name() const override { return inner_->name(); }
    bool execute(Context& ctx) override {
        for (int i = 1; i <= attempts_; ++i) {
            if (inner_->execute(ctx)) return true;
            std::cout << "    (attempt " << i << "/" << attempts_ << " failed)\n";
        }
        return false;
    }
private:
    std::unique_ptr<Task> inner_;
    int attempts_;
};

// ---------------------------------------------------------------- Observer
class Observer {
public:
    virtual ~Observer() = default;
    virtual void onEvent(const std::string& event, const std::string& detail) = 0;
};

class ConsoleLogger : public Observer {
public:
    void onEvent(const std::string& event, const std::string& detail) override {
        std::cout << "[" << event << "] " << detail << "\n";
    }
};

// ---------------------------------------------------------------- Workflow (the engine)
class Workflow {
public:
    explicit Workflow(std::string name) : name_(std::move(name)) {}
    void addTask(std::unique_ptr<Task> t) { tasks_.push_back(std::move(t)); }
    void addObserver(Observer* o) { observers_.push_back(o); }

    bool run(Context& ctx) {
        notify("WORKFLOW_START", name_);
        int step = 1;
        for (auto& t : tasks_) {
            notify("TASK_START", std::to_string(step) + ". " + t->name());
            if (!t->execute(ctx)) {
                notify("TASK_FAILED", t->name());
                notify("WORKFLOW_FAILED", name_);
                return false;
            }
            notify("TASK_DONE", t->name());
            ++step;
        }
        notify("WORKFLOW_DONE", name_);
        return true;
    }

private:
    void notify(const std::string& e, const std::string& d) {
        for (auto* o : observers_) o->onEvent(e, d);
    }
    std::string name_;
    std::vector<std::unique_ptr<Task>> tasks_;
    std::vector<Observer*> observers_;
};

// ---------------------------------------------------------------- Builder
class WorkflowBuilder {
public:
    explicit WorkflowBuilder(const std::string& name) : wf_(std::make_unique<Workflow>(name)) {}

    WorkflowBuilder& add(const std::string& type, const Params& p = {}, int retries = 1) {
        auto task = TaskFactory::instance().create(type, p);
        if (retries > 1) task = std::make_unique<RetryTask>(std::move(task), retries);
        wf_->addTask(std::move(task));
        return *this;
    }
    WorkflowBuilder& observe(Observer* o) { wf_->addObserver(o); return *this; }
    std::unique_ptr<Workflow> build() { return std::move(wf_); }

private:
    std::unique_ptr<Workflow> wf_;
};

// ---------------------------------------------------------------- Demo
int main() {
    ConsoleLogger logger;
    Context ctx;

    auto wf = WorkflowBuilder("Onboard new student")
                  .observe(&logger)
                  .add("set_var", {{"key", "student"}, {"value", "Priya"}})
                  .add("print",   {{"message", "Welcome, ${student}!"}})
                  .add("flaky",   {{"fail_times", "2"}}, /*retries=*/3)
                  .add("print",   {{"message", "Account created for ${student}"}})
                  .build();

    wf->run(ctx);
    return 0;
}
