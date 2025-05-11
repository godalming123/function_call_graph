#define __USE_POSIX
#define _POSIX_C_SOURCE 200809L
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <unistd.h>
#include <iostream>
#include <sys/stat.h>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <sys/mman.h>
#include <cstring>


// Forwards
struct CSSym;
struct CSFile;
struct CSFuncCall;

// Hash of symbols
typedef std::unordered_map<std::string, const CSSym *> CSSymHash;
typedef std::unordered_map<std::string, std::vector<const CSFuncCall *>> CSDB;

// Symbol: could be a function definition or function call
class CSSym
{
public:
    CSSym(const char *name, char mark, size_t line, const CSFile *file) :
        _name(name), _mark(mark), _line(line), _file(file) {}

    char getMark() const { return _mark; }
    virtual std::string getName() const { return _name; }

private:
    std::string        _name;
    char          _mark;
    size_t        _line;
    const CSFile *_file;
    CSSymHash     _fn_defs; // Function definitions
};

// Symbol: could be a function definition or function call
class CSFuncCall : public CSSym
{
public:
    CSFuncCall(const char *name, char mark, size_t line, const CSFile *file) :
        CSSym(name, mark, line, file) {}
};

// Symbol: could be a function definition or function call
class CSFuncDef : public CSSym
{
public:
    CSFuncDef(const char *name, char mark, size_t line, const CSFile *file) :
        CSSym(name, mark, line, file) {}

    void getCallees(std::vector<const CSFuncCall *> &addem) const {
        for (auto callee: _callees)
          addem.push_back(static_cast<const CSFuncCall *>(callee.second));
    }

    // Unique add
    void addCallee(const CSFuncCall *fncall) {
        auto pr = std::make_pair<std::string, const CSSym *>(fncall->getName(), fncall);
        _callees.insert(pr);
    }

private:
        CSSymHash _callees; // Function calls
};

// A file entry contains a list of symbols, we only collect function calls here.
class CSFile
{
public:
    CSFile(const char *name, char mark):
        _name(name), _mark(mark), _current_fndef(nullptr) {}

    CSFuncDef *getCurrentFunction() const { return _current_fndef; }
    std::string getName() const { return _name; }
    const CSSymHash *getFunctions() { return &_functions; }
    size_t getFunctionCount() const { return _functions.size(); }

    void addFunctionDef(CSFuncDef *fndef) {
        auto pr = std::make_pair<std::string, const CSSym *>(fndef->getName(), fndef);
        _functions.insert(pr);
        _current_fndef = fndef;
    }

private:
    std::string    _name;
    char      _mark;
    CSSymHash _functions;

    // The current function being added to (callees being added).
    CSFuncDef *_current_fndef;
};

// cscope database (cscope.out) header
struct CSHeader
{
    int         version;
    bool        compression;    /* -c */
    bool        inverted_index; /* -q */
    bool        prefix_match;   /* -T */
    size_t      syms_start;
    size_t      trailer;
    const char *dir;
};

//cscope database (cscope.out) trailer
struct CSTrailer
{
    int    n_viewpaths;
    char **viewpath_dirs;
    int    n_srcs;
    char **srcs;
    int    n_incs;
    char **incs;
};

// cscope database, contains a list of file entries
struct CS
{
public:
    CS(const char *fname);
    void addFile(CSFile *f) { _files.push_back(f); }
    CSDB *buildDatabase();

private:
    CSHeader               _hdr;
    CSTrailer              _trailer;
    const char            *_name;
    int                    _n_functions;
    std::vector<CSFile *>  _files;

    void initHeader(const uint8_t *data, size_t data_size);
    void initTrailer(const uint8_t *data, size_t data_size);
    void initSymbols(const uint8_t *data, size_t data_size);
    void loadCScope();
};


#define CS_FN_DEF  '$'
#define CS_FN_CALL '`'
static const char cs_marks[] =
{
    '@', CS_FN_DEF, CS_FN_CALL,
    '}', '#', ')',
    '~', '=', ';',
    'c', 'e', 'g',
    'l', 'm', 'p',
    's', 't', 'u'
};


// Public routines
extern void csPrintCallers(FILE *out, CSDB *db, const char *fn_name, int depth);
extern void csPrintCallees(FILE *out, CSDB *db, const char *fn_name, int depth);

using std::cout;
using std::endl;

typedef struct { size_t off; size_t data_len; const uint8_t *data; } pos_t;

// Cscope data stream accessors
#define VALID(_p)     ((_p)->off <= (_p)->data_len)
#define NXT_VALID(_p) ((_p)->off+1 <= (_p)->data_len)
#define END(_p)       ((_p)->off >= (_p)->data_len)
#define CH(_p)        (VALID(_p) ? (_p)->data[(_p)->off] : EOF)

// Error handling and debugging
#define ERR(...) do {fprintf(stderr,__VA_ARGS__);fputc('\n', stderr);} while(0)
#ifdef DEBUG
#define DBG(...) do {fprintf(stdout,__VA_ARGS__);fputc('\n', stdout);} while(0)
#else
#define DBG(...)
#endif

static void getLine(pos_t *pos, char *buf, size_t buf_len)
{
    size_t len, st = pos->off;

    // Get initial line (header line)
    while (CH(pos) != EOF && CH(pos) != '\n')
      ++pos->off;

    len = pos->off - st; // Don't return the '\n'
    ++pos->off;          // +1 to advance to the '\n'
    if (END(pos) || len > buf_len)
      return;

    memcpy(buf, pos->data + st, len);
    buf[len] = '\0';
}

static CSFile *newFile(const char *str)
{
    const char *c = str;

    // Skip whitespace
    while (isspace(*c))
      ++c;

    return new CSFile(c+1, *c);
}

static bool isMark(char c)
{
    unsigned i;
    for (i=0; i<sizeof(cs_marks)/sizeof(cs_marks[0]); ++i)
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
static void loadSymbolsInFile(
    CSFile *file,
    pos_t  *pos,
    long    lineno)
{
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
        if (isMark(c[0]) && strlen(c+1) == 0)
          continue;

        if (mark == CS_FN_CALL) {
            CSFuncDef *fndef = file->getCurrentFunction();

            // This is probably a macro
            if (!fndef)
              continue;

            // We always allocate a new symbol
            // so the next pointer for a call will be used as a
            // list of all calls that the function defintiion makes.
            fndef->addCallee(new CSFuncCall(c, mark, lineno, file));
        }
        else if (mark == CS_FN_DEF) {
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
static void fileLoadSymbols(CSFile *file, pos_t *pos)
{
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
CS::CS(const char *fname)
{
    FILE *fp;
    uint8_t *data;
    struct stat st;

    if (!(fp = fopen(fname, "r")))
      throw("Could not open cscope database file");

    // mmap the input cscope database
    fstat(fileno(fp), &st);
    data = (uint8_t *)mmap(NULL, st.st_size,
                           PROT_READ, MAP_PRIVATE, fileno(fp), 0);
    if (!data)
      throw("Error memory maping cscope database");

    // Initialize the data
    initHeader(data, st.st_size);
    initTrailer(data, st.st_size);
    initSymbols(data, st.st_size);

    // Done loading data
    munmap(data, st.st_size);
    fclose(fp);
}

// Create a database
CSDB *CS::buildDatabase()
{
    int i = 0, sidx = 0;
    const char spin[] = "-\\|/";
    auto db = new CSDB;

    cout << "Building internal database: ";
    for (auto f: this->_files) {
        for (auto fndef_pr: *f->getFunctions()) {
            // Collect all calls this function (fndef) makes
            std::vector<const CSFuncCall *> callees;
            auto fndef = static_cast<const CSFuncDef *>(fndef_pr.second);
            fndef->getCallees(callees);

            // Add the funtion_def : calleess map entry
            auto pr = std::make_pair(fndef->getName(), callees);
            db->insert(pr);
            if ((++i % 1000) == 0)
              cout << '\b' << spin[sidx++ % 4] << std::flush;
        }
    }

    cout << '\b' << " Done " << endl;
    return db;
}

// Does a call b?
static bool isCallerOf(CSDB *db, const char *a, const char *b)
{
    // All the functions 'a' calls
    for (auto callee: (*db)[a]) {
        if (strcmp(callee->getName().c_str(), b) == 0)
            return true;
    }

    return false;
}

// Collect all of the callers to 'fn_name'
static void printCallersRec(
    FILE       *out,
    CSDB       *db,
    const char *fn_name,
    int         depth)
{
    if (depth <= 0)
      return;

    for (auto pr: *db) {
        const char *item = pr.first.c_str();
        // Does 'item' call 'fn_name' ?
        if (isCallerOf(db, item, fn_name)) {
            fprintf(out, "    %s -> %s\n", item, fn_name);
            printCallersRec(out, db, item, depth - 1);
        }
    }
}

void csPrintCallers(FILE *out, CSDB *db, const char *fn_name, int depth)
{
    cout << "Building callers... " << std::flush;
    fprintf(out, "digraph \"Callers to %s\" {\n", fn_name);
    printCallersRec(out, db, fn_name, depth);
    fprintf(out, "}\n");
    cout << "Done" << endl;
}

// Collect all of the callees to 'fn_name'
static void printCalleesRec(
    FILE       *out,
    CSDB       *db,
    const char *fn_name,
    int         depth)
{
    if (depth <= 0)
      return;

    for (auto callee: (*db)[fn_name]) {
        fprintf(out, "    %s -> %s\n", fn_name, callee->getName().c_str());
        printCalleesRec(out, db, callee->getName().c_str(), depth - 1);
    }
}

void csPrintCallees(FILE *out, CSDB *db, const char *fn_name, int depth)
{
    cout << "Building callees... " << std::flush;
    fprintf(out, "digraph \"Callees of %s\" {\n", fn_name);
    printCalleesRec(out, db, fn_name, depth);
    fprintf(out, "}\n");
    cout << "Done" << endl;
}

// Header looks like:
//     <cscope> <dir> <version> [-c] [-q <symbols>] [-T] <trailer>
void CS::initHeader(const uint8_t *data, size_t data_len)
{
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
              this->_hdr.inverted_index = true; // TODO
            else {
                ERR("Unrecognized header option");
                return;
            }
        }
        else {
            this->_hdr.trailer = atol(tok);
            break;
        }
    }
}

void CS::initTrailer(const uint8_t *data, size_t data_len)
{
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
    for (i=0; i<this->_trailer.n_viewpaths; ++i) {
        getLine(&pos, line, sizeof(line));
        DBG("[%d of %d] Viewpath: %s", i+1, this->_trailer.n_viewpaths, line);
    }

    // Sources
    getLine(&pos, line, sizeof(line));
    this->_trailer.n_srcs = atoi(line);
    for (i=0; i<this->_trailer.n_srcs; ++i) {
        getLine(&pos, line, sizeof(line));
        DBG("[%d of %d] Sources: %s", i+1, this->_trailer.n_srcs, line);
    }

    // Includes
    getLine(&pos, line, sizeof(line));
    this->_trailer.n_incs = atoi(line);
    getLine(&pos, line, sizeof(line));
    for (i=0; i<this->_trailer.n_incs; ++i) {
        getLine(&pos, line, sizeof(line));
        DBG("[%d of %d] Includes: %s", i+1, this->_trailer.n_incs, line);
    }
}

void CS::initSymbols(const uint8_t *data, size_t data_len)
{
    pos_t pos = {0};
    char line[1024];
    CSFile *file;

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
        this->addFile(file);
        this->_n_functions += file->getFunctionCount();
    }
}

static void usage(const char *execname)
{
    printf("Usage: %s -c cscope.out -f fn_name "
           "[-o outputfile] [-d depth] <-x | -y>\n"
           "  -c cscope.out: cscope.out database file\n"
           "  -f fn_name:    Function name to plot callers of\n"
           "  -d depth:      Depth of traversal.\n"
           "  -o outputfile: Write results to outputfile.\n"
           "  -x:            Do not print callers of fn_name.\n"
           "  -y:            Do not print callees of fn_name.\n"
           "  -h:            This help message.\n",
           execname);
    exit(EXIT_SUCCESS);
}

int main(int argc, char **argv)
{
    int opt;
    FILE *out;
    bool do_callees, do_callers;
    const char *fname, *fn_name, *out_fname;
    CS *cs;
    CSDB *db;
    int depth = 2;

    do_callers = true;
    do_callees = true;
    fname = NULL;
    out_fname = NULL;
    fn_name = NULL;

    while ((opt = getopt(argc, argv, "c:d:f:o:hxy")) != -1) {
        switch (opt) {
        case 'c': fname = optarg; break;
        case 'd': depth = atoi(optarg); break;
        case 'f': fn_name = optarg; break;
        case 'o': out_fname = optarg; break;
        case 'x': do_callers = false; break;
        case 'y': do_callees = false; break;
        case 'h': usage(argv[0]); break;
        default: return EXIT_FAILURE;
        }
    }

    if (!fname || !fn_name || depth < 0) {
        fprintf(stderr, "Invalid options, see '-h'\n");
        return EXIT_FAILURE;
    }

    if (out_fname && !(out = fopen(out_fname, "w"))) {
        fprintf(stderr, "Error opening output file %s: %s\n",
                out_fname, strerror(errno));
        exit(errno);
    }
    else if (!out_fname)
      out = stdout;

    // Load
    try {
        cs = new CS(fname);
    } 
    catch (const char *err) {
        fprintf(stderr, "Error loading cscope database: %s\n", err);
        exit(EXIT_FAILURE);
    }
    db = cs->buildDatabase();

    // Go!
    if (do_callers)
      csPrintCallers(out, db, fn_name, depth);
    if (do_callees)
      csPrintCallees(out, db, fn_name, depth);

    fclose(out);
    return 0;
}
