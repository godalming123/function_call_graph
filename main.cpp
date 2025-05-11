#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <iostream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

bool logging = false;

// Global variables for spinners
bool spinning = false;
std::string spinnerSpinningMessage = "";
std::string spinnerDoneMessage = "";
std::thread spinnerThread;

// Uses unicodes braille charecters
constexpr const char* spinnerChars[8] = {"⣾", "⣽", "⣻", "⢿",
                                         "⡿", "⣟", "⣯", "⣷"};

// ANSI control codes
constexpr const char* hideCursor = "\u001b[?25l";
constexpr const char* showCursor = "\u001b[?25h";
constexpr const char* goToLineStart = "\u001b[0E";
constexpr const char* clearLineAfterCursor = "\u001b[0K";

void spin() {
    std::cout << hideCursor;
    for (int i = 0; spinning; i++) {
        i = i % (8 * 33);
        if ((i % 33) == 0) {
            std::cout << goToLineStart << spinnerChars[i % 8] << " "
                      << spinnerSpinningMessage << std::flush;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    std::cout << goToLineStart << clearLineAfterCursor << spinnerDoneMessage
              << std::endl
              << std::flush << showCursor;
}

void startSpinner(std::string spinningMessage, std::string doneMessage) {
    if (logging) {
        spinnerSpinningMessage = spinningMessage;
        spinnerDoneMessage = doneMessage;
        spinning = true;
        spinnerThread = std::thread(&spin);
    }
}

void stopSpinner() {
    spinning = false;
    spinnerThread.join();
}

// Forwards
struct CSSym;
struct CSFile;
struct CSFuncCall;

// Hash of symbols
typedef std::unordered_map<std::string, const CSSym*> CSSymHash;
typedef std::unordered_map<std::string, std::vector<const CSFuncCall*>> CSDB;

// Symbol: could be a function definition or function call
class CSSym {
   public:
    CSSym(const char* name, char mark, size_t line, const CSFile* file)
        : _name(name), _mark(mark), _line(line), _file(file) {}

    char getMark() const { return _mark; }
    virtual std::string getName() const { return _name; }

   private:
    std::string _name;
    char _mark;
    size_t _line;
    const CSFile* _file;
    CSSymHash _fn_defs;  // Function definitions
};

// Symbol: could be a function definition or function call
class CSFuncCall : public CSSym {
   public:
    CSFuncCall(const char* name, char mark, size_t line, const CSFile* file)
        : CSSym(name, mark, line, file) {}
};

// Symbol: could be a function definition or function call
class CSFuncDef : public CSSym {
   public:
    CSFuncDef(const char* name, char mark, size_t line, const CSFile* file)
        : CSSym(name, mark, line, file) {}

    void getCallees(std::vector<const CSFuncCall*>& addem) const {
        for (auto callee : _callees)
            addem.push_back(static_cast<const CSFuncCall*>(callee.second));
    }

    // Unique add
    void addCallee(const CSFuncCall* fncall) {
        auto pr = std::make_pair<std::string, const CSSym*>(fncall->getName(),
                                                            fncall);
        _callees.insert(pr);
    }

   private:
    CSSymHash _callees;  // Function calls
};

// A file entry contains a list of symbols, we only collect function calls here.
class CSFile {
   public:
    CSFile(const char* name, char mark)
        : _name(name), _mark(mark), _current_fndef(nullptr) {}

    CSFuncDef* getCurrentFunction() const { return _current_fndef; }
    std::string getName() const { return _name; }
    const CSSymHash* getFunctions() { return &_functions; }
    size_t getFunctionCount() const { return _functions.size(); }

    void addFunctionDef(CSFuncDef* fndef) {
        auto pr =
            std::make_pair<std::string, const CSSym*>(fndef->getName(), fndef);
        _functions.insert(pr);
        _current_fndef = fndef;
    }

   private:
    std::string _name;
    char _mark;
    CSSymHash _functions;

    // The current function being added to (callees being added).
    CSFuncDef* _current_fndef;
};

// cscope database (cscope.out) header
struct CSHeader {
    int version;
    bool compression;    /* -c */
    bool inverted_index; /* -q */
    bool prefix_match;   /* -T */
    size_t syms_start;
    size_t trailer;
    const char* dir;
};

// cscope database (cscope.out) trailer
struct CSTrailer {
    int n_viewpaths;
    char** viewpath_dirs;
    int n_srcs;
    char** srcs;
    int n_incs;
    char** incs;
};

// cscope database, contains a list of file entries
struct CS {
   public:
    CS(FILE* fp);
    std::vector<CSFile*> files;
    CSDB* db;

   private:
    CSHeader _hdr;
    CSTrailer _trailer;
    const char* _name;
    int _n_functions;

    void initHeader(const uint8_t* data, size_t data_size);
    void initTrailer(const uint8_t* data, size_t data_size);
    void initSymbols(const uint8_t* data, size_t data_size);
    void loadCScope();
};

#define CS_FN_DEF '$'
#define CS_FN_CALL '`'
static const char cs_marks[] = {'@', CS_FN_DEF, CS_FN_CALL, '}', '#', ')',
                                '~', '=',       ';',        'c', 'e', 'g',
                                'l', 'm',       'p',        's', 't', 'u'};

typedef struct {
    size_t off;
    size_t data_len;
    const uint8_t* data;
} pos_t;

// Cscope data stream accessors
#define VALID(_p) ((_p)->off <= (_p)->data_len)
#define NXT_VALID(_p) ((_p)->off + 1 <= (_p)->data_len)
#define END(_p) ((_p)->off >= (_p)->data_len)
#define CH(_p) (VALID(_p) ? (_p)->data[(_p)->off] : EOF)

// Error handling and debugging
#define ERR(...)                      \
    if (logging) {                    \
        fprintf(stderr, __VA_ARGS__); \
        fputc('\n', stderr);          \
    }                                 \
    while (0)
#ifdef DEBUG
#define DBG(...)                      \
    if (logging) {                    \
        fprintf(stdout, __VA_ARGS__); \
        fputc('\n', stdout);          \
    }                                 \
    while (0)
#else
#define DBG(...)
#endif

static void getLine(pos_t* pos, char* buf, size_t buf_len) {
    size_t len, st = pos->off;

    // Get initial line (header line)
    while (CH(pos) != EOF && CH(pos) != '\n')
        ++pos->off;

    len = pos->off - st;  // Don't return the '\n'
    ++pos->off;           // +1 to advance to the '\n'
    if (END(pos) || len > buf_len)
        return;

    memcpy(buf, pos->data + st, len);
    buf[len] = '\0';
}

static CSFile* newFile(const char* str) {
    const char* c = str;

    // Skip whitespace
    while (isspace(*c))
        ++c;

    return new CSFile(c + 1, *c);
}

static bool isMark(char c) {
    unsigned i;
    for (i = 0; i < sizeof(cs_marks) / sizeof(cs_marks[0]); ++i)
        if (c == cs_marks[i])
            return true;
    return false;
}

// Parse each line in the <file mark><file path>:
// From docs:
//
// and for each source line containing a symbol
//
// <line number><blank><non-symbol text>
// <optional mark><symbol>
// <non-symbol text>
// repeat above 2 lines as necessary
// <empty line>
//
// Source:
// ftp://ftp.eeng.dcu.ie/pub/ee454/cygwin/usr/share/doc/mlcscope-14.1.8/html/cscope.html
//
// Returns: function definition that was just added, or is being added to.
static void loadSymbolsInFile(CSFile* file, pos_t* pos, long lineno) {
    char line[1024], *c, mark;

    // Suck in only function calls or definitions for this lineno
    while (VALID(pos)) {
        // Skip <non-symbol text> and obtain:
        //
        // <optional mark><symbol text>
        // This will be <blank> if end of symbol data.
        getLine(pos, line, sizeof(line));
        if (strlen(line) == 0)
            break;

        c = line;

        // Skip spaces and not tabs
        while (*c == ' ')
            ++c;

        // <optional mark>
        mark = 0;
        if (c[0] == '\t' && isMark(c[1])) {
            mark = c[1];
            c += 2;
        }

        // Only accept function definitions or function calls
        if (!mark || (mark != CS_FN_DEF && mark != CS_FN_CALL))
            continue;

        // Skip lines only containing mark characters
        if (isMark(c[0]) && strlen(c + 1) == 0)
            continue;

        if (mark == CS_FN_CALL) {
            CSFuncDef* fndef = file->getCurrentFunction();

            // This is probably a macro
            if (!fndef)
                continue;

            // We always allocate a new symbol
            // so the next pointer for a call will be used as a
            // list of all calls that the function defintiion makes.
            fndef->addCallee(new CSFuncCall(c, mark, lineno, file));
        } else if (mark == CS_FN_DEF) {
            // Add fn definition to file: Most recently defined is first
            file->addFunctionDef(new CSFuncDef(c, mark, lineno, file));
        }

        if (strlen(c) == 0)
            continue;

        // <non-symbol text>
        getLine(pos, line, sizeof(line));
    }
}

// Extract the symbols for file
// This must start with the <mark><file> line.
static void fileLoadSymbols(CSFile* file, pos_t* pos) {
    long lineno;
    char line[1024], *c;

    DBG("Loading: %s", file->name);

    // <empty line>
    getLine(pos, line, sizeof(line));

    // Now parse symbol information for eack line in 'file'
    while (VALID(pos)) {
        // Either this is a symbol, or a file.  If this is a file, then we are
        // done processing the current file.  So, in that case we break and
        // restore the position at the start of the next file's data:
        // <mark><file> line.
        //
        // So there are two cases here:
        // 1) New set of symbols: <lineno><blank><non-symbol text>
        // 2) A new file: <mark><file>
        getLine(pos, line, sizeof(line));
        c = line;
        while (isspace(*c))
            ++c;

        // Case 2: New file
        if (c[0] == '@') {
            pos->off -= strlen(line);
            return;
        }

        // Case 1: Symbols at line!
        // <line number><blank>
        lineno = atol(c);
        loadSymbolsInFile(file, pos, lineno);
    }
}

// Load a cscope database and return a pointer to the data
CS::CS(FILE* fp) {
    uint8_t* data;
    struct stat st;

    // mmap the input cscope database
    fstat(fileno(fp), &st);
    data =
        (uint8_t*)mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fileno(fp), 0);
    if (data == NULL) {
        std::cerr << "Error memory maping cscope database" << std::endl;
        exit(errno);
    }

    // Initialize the data
    initHeader(data, st.st_size);
    initTrailer(data, st.st_size);
    initSymbols(data, st.st_size);

    // Done loading data
    munmap(data, st.st_size);
    fclose(fp);

    // Build database
    this->db = new CSDB;

    startSpinner("Building internal database", "Built internal database");
    for (auto f : this->files) {
        for (auto fndef_pr : *f->getFunctions()) {
            // Collect all calls this function (fndef) makes
            std::vector<const CSFuncCall*> callees;
            auto fndef = static_cast<const CSFuncDef*>(fndef_pr.second);
            fndef->getCallees(callees);

            // Add the funtion_def : calleess map entry
            auto pr = std::make_pair(fndef->getName(), callees);
            this->db->insert(pr);
        }
    }
    stopSpinner();
}

// Does a call b?
static bool isCallerOf(CSDB* db, const char* a, const char* b) {
    // All the functions 'a' calls
    for (auto callee : (*db)[a]) {
        if (strcmp(callee->getName().c_str(), b) == 0)
            return true;
    }

    return false;
}

// Collect all of the callers to 'fn_name'
static std::string getCallersRec(CSDB* db, const char* fn_name, int depth) {
    if (depth <= 0)
        return "";
    std::string out = "";
    for (auto pr : *db) {
        const char* item = pr.first.c_str();
        // Does 'item' call 'fn_name' ?
        if (isCallerOf(db, item, fn_name)) {
            out.append(std::format("    {} -> {}\n", item, fn_name));
            out.append(getCallersRec(db, item, depth - 1));
        }
    }
    return out;
}

// Collect all of the callees to 'fn_name'
static std::string getCalleesRec(CSDB* db, const char* fn_name, int depth) {
    if (depth <= 0)
        return "";
    std::string out = "";
    for (auto callee : (*db)[fn_name]) {
        out.append(std::format("    {} -> {}\n", fn_name, callee->getName()));
        out.append(getCalleesRec(db, callee->getName().c_str(), depth - 1));
    }
    return out;
}

// Header looks like:
//     <cscope> <dir> <version> [-c] [-q <symbols>] [-T] <trailer>
void CS::initHeader(const uint8_t* data, size_t data_len) {
    pos_t pos = {0};
    char buf[1024], *tok;

    pos.data = data;
    pos.data_len = data_len;
    getLine(&pos, buf, sizeof(buf));

    // After the header are the symbols
    this->_hdr.syms_start = pos.off;

    // Load in the header: <cscope>
    tok = strtok(buf, " ");
    if (strncmp(tok, "cscope", strlen("cscope"))) {
        ERR("This does not appear to be a cscope database");
        return;
    }

    // Version
    this->_hdr.version = atoi(strtok(NULL, " "));

    // Directory
    this->_hdr.dir = strndup(strtok(NULL, " "), 1024);

    // Optionals: [-c] [-T] [-q <syms>]
    while ((tok = strtok(NULL, " "))) {
        if (tok[0] == '-' && strlen(tok) == 2) {
            if (tok[1] == 'c')
                this->_hdr.compression = true;
            else if (tok[1] == 'T')
                this->_hdr.prefix_match = true;
            else if (tok[1] == 'q')
                this->_hdr.inverted_index = true;  // TODO
            else {
                ERR("Unrecognized header option");
                return;
            }
        } else {
            this->_hdr.trailer = atol(tok);
            break;
        }
    }
}

void CS::initTrailer(const uint8_t* data, size_t data_len) {
    int i;
    char line[1024] = {0};
    pos_t pos = {0};

    pos.data = data;
    pos.data_len = data_len;
    pos.off = this->_hdr.trailer;

    if (!VALID(&pos))
        return;

    // Viewpaths
    getLine(&pos, line, sizeof(line));
    this->_trailer.n_viewpaths = atoi(line);
    for (i = 0; i < this->_trailer.n_viewpaths; ++i) {
        getLine(&pos, line, sizeof(line));
        DBG("[%d of %d] Viewpath: %s", i + 1, this->_trailer.n_viewpaths, line);
    }

    // Sources
    getLine(&pos, line, sizeof(line));
    this->_trailer.n_srcs = atoi(line);
    for (i = 0; i < this->_trailer.n_srcs; ++i) {
        getLine(&pos, line, sizeof(line));
        DBG("[%d of %d] Sources: %s", i + 1, this->_trailer.n_srcs, line);
    }

    // Includes
    getLine(&pos, line, sizeof(line));
    this->_trailer.n_incs = atoi(line);
    getLine(&pos, line, sizeof(line));
    for (i = 0; i < this->_trailer.n_incs; ++i) {
        getLine(&pos, line, sizeof(line));
        DBG("[%d of %d] Includes: %s", i + 1, this->_trailer.n_incs, line);
    }
}

void CS::initSymbols(const uint8_t* data, size_t data_len) {
    pos_t pos = {0};
    char line[1024];
    CSFile* file;

    pos.off = this->_hdr.syms_start;
    pos.data = data;
    pos.data_len = data_len;

    while (VALID(&pos) && pos.off <= this->_hdr.trailer) {
        // Get file info
        getLine(&pos, line, sizeof(line));
        file = newFile(line);
        fileLoadSymbols(file, &pos);

        // No-name file
        if (file->getName().size() == 0) {
            delete file;
            continue;
        }

        // Add the file to the list of files
        this->files.push_back(file);
        this->_n_functions += file->getFunctionCount();
    }
}

static void usage(const char* execname) {
    std::cerr
        << "Usage: " << execname
        << " function_name [i input_file] [o output_file] [d depth] [x|y]\n"
           "  i input_file:  cscope database file, defaults to using stdin\n"
           "  d depth:       Depth of traversal, defaults to 5\n"
           "  o output_file: File to write results to, defaults to stdout\n"
           "  x:             Do not print callers of function_name\n"
           "  y:             Do not print callees of function_name\n";
    exit(EXIT_FAILURE);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Got too few arguments" << std::endl;
        usage(argv[0]);
    }

    FILE* out = stdout;
    FILE* in = stdin;
    int depth = 5;

    bool outputSpecified = false;
    bool inputSpecified = false;
    bool depthSpecified = false;

    bool do_callers = true;
    bool do_callees = true;

    for (int i = 2; i < argc; i++) {
        if (strlen(argv[i]) != 1) {
            std::cerr << "Expected `" << argv[i] << "` to be 1 charecter long";
            usage(argv[0]);
        }
        int option = argv[i][0];
        bool haveExtraArg = i < argc - 1;
        if (option == 'x') {
            do_callers = false;
        } else if (option == 'y') {
            do_callees = false;
        } else if (option == 'd' && haveExtraArg && !depthSpecified) {
            depthSpecified = true;
            i++;
            depth = atoi(argv[i]);
            if (depth <= 0) {
                std::cerr << "Depth must be greater than 0" << std::endl;
                return EXIT_FAILURE;
            }
        } else if (option == 'o' && haveExtraArg && !outputSpecified) {
            outputSpecified = true;
            i++;
            logging = true;
            out = fopen(argv[i], "w");
            if (out == NULL) {
                fprintf(stderr, "Error opening output file %s: %s\n", argv[i],
                        strerror(errno));
                exit(errno);
            };
        } else if (option == 'i' && haveExtraArg && !inputSpecified) {
            inputSpecified = true;
            i++;
            in = fopen(argv[i], "r");
            if (in == NULL) {
                std::cerr << "Could not open cscope database file called `"
                          << argv[i] << "`" << std::endl;
                return errno;
            }
        } else {
            std::cerr << "Unexpected option " << option << std::endl;
            usage(argv[0]);
        }
    }

    // Load
    CS* cs = new CS(in);

    // Go!
    const char* func_name = argv[1];
    if (do_callers) {
        startSpinner("Building callers", "Built callers");
        std::string callers = getCallersRec(cs->db, func_name, depth);
        if (callers.length() > 0) {
            fprintf(out, "digraph \"Callers to %s\" {\n%s}\n", func_name,
                    callers.c_str());
        }
        stopSpinner();
    }
    if (do_callees) {
        startSpinner("Building callees", "Built callees");
        std::string callees = getCalleesRec(cs->db, func_name, depth);
        if (callees.length() > 0) {
            fprintf(out, "digraph \"Callees of %s\" {\n%s}\n", func_name,
                    callees.c_str());
        }
        stopSpinner();
    }

    fclose(out);
    return 0;
}
