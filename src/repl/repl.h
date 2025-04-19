#ifndef QUANTA_REPL_H
#define QUANTA_REPL_H

#include "interpreter.h"
#include <memory>

namespace quanta {

class REPL {
public:
    explicit REPL(std::shared_ptr<Interpreter> interpreter);
    void start();

private:
    void displayHelp();
    std::shared_ptr<Interpreter> interpreter_;
};

} // namespace quanta

#endif // QUANTA_REPL_H 