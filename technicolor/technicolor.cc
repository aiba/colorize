// Copyright 2007 Aaron B. Iba
//
// Author: Aaron B. Iba (aiba@alum.mit.edu)
//
// Notes:
//  * awesome color reference: http://linuxgazette.net/issue65/padala.html
//  * pipes: http://www.unixwiz.net/techtips/remap-pipe-fds.html
//  * captures with boost::regexp: http://www.boost.org/libs/regex/doc/captures.html
//

#include <assert.h>
#include <boost/regex.hpp>
#include <fstream>
#include <iostream>
#include <list>
#include <sstream>
#include <stdio.h>
#include <string>
#include <sys/select.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

using std::cerr;
using std::cout;
using std::endl;
using std::ifstream;
using std::min;
using std::string;
using std::stringstream;
using std::vector;

//----------------------------------------------------------------
// terminal color constants
//----------------------------------------------------------------

static const int RESET     = 0;
static const int BRIGHT    = 1;
static const int DIM       = 2;
static const int UNDERLINE = 3;
static const int BLINK     = 4;
static const int REVERSE   = 7;
static const int HIDDEN    = 8;

static const int BLACK     = 0;
static const int RED       = 1;
static const int GREEN     = 2;
static const int YELLOW    = 3;
static const int BLUE      = 4;
static const int MAGENTA   = 5;
static const int CYAN      = 6;
static const int WHITE     = 7;

#define CASE_RETURN(X, S)			                  \
  {								  \
    string temp = #S;						  \
    transform(temp.begin(), temp.end(), temp.begin(), ::tolower); \
    if (X == temp) {						  \
      return S;							  \
    }								  \
  }

int lookup_attr_const(string attr) {
  CASE_RETURN(attr, RESET);
  CASE_RETURN(attr, BRIGHT);
  CASE_RETURN(attr, DIM);
  CASE_RETURN(attr, UNDERLINE);
  CASE_RETURN(attr, BLINK);
  CASE_RETURN(attr, REVERSE);
  CASE_RETURN(attr, HIDDEN);
  cerr << "technicolor config error: unknown value [" << attr << "]." << endl;
  return -1;
}

int lookup_color_const(string color) {
  CASE_RETURN(color, BLACK);
  CASE_RETURN(color, RED);
  CASE_RETURN(color, GREEN);
  CASE_RETURN(color, YELLOW);
  CASE_RETURN(color, BLUE);
  CASE_RETURN(color, MAGENTA);
  CASE_RETURN(color, CYAN);
  CASE_RETURN(color, WHITE);
  cerr << "technicolor config error: unknown value [" << color << "]." << endl;
  return -1;
}

//----------------------------------------------------------------
// globals
//----------------------------------------------------------------

static const bool         DEBUG                = false;
static const suseconds_t  SELECT_TIMEOUT_USECS = 500;
static const suseconds_t  SLEEP_INTERVAL_USECS = 10000;

static const string       USAGE = \
  "USAGE: technicolor [--config /path/to/config/file] command [command_arg ...]\n";

#define MAX_LINEBUFFER_SIZE 32768

//----------------------------------------------------------------
// data types
//----------------------------------------------------------------

struct color_spec {
  // assign value of -1 for any of these field values to indicate
  // "unspecified", in which case the values will inherit from the
  // default.
  int attr;
  int foreground;
  int background;
};

struct line_spec {
  boost::regex regexp;
  vector<color_spec> color_list;
};

struct technicolor_config {
  color_spec stdout_props;
  color_spec stderr_props;
  vector<line_spec> spec_list;
};

//----------------------------------------------------------------
// prototypes
//----------------------------------------------------------------

void parse_command_line(int argc,
			char** argv,
			technicolor_config* config,
			char*** child_argv);

void load_config_file(string filename, technicolor_config* config);
void fill_default_config(technicolor_config* config);
void parse_config_file(const vector<string>& lines, technicolor_config* config);
void parse_line_spec(const string& line, technicolor_config* config);
bool parse_color_spec(const string& str, color_spec* cs);
bool parse_color_spec_part(const string& str, color_spec* cs);

bool read_bytes_available(int input_fd, string& line_buffer);

bool flush_complete_lines(const technicolor_config& config,
			  string& buffer,
			  FILE* output_file);

void write_colored_line(const technicolor_config& config,
			const string& line_buffer,
			FILE* output_file);

bool match_covers_line(const string& line,
		       vector<color_spec> color_list,
		       const boost::smatch& what);

bool bytes_ready_to_read(int fd);
void debug(string message);

void textcolor(FILE* f, const color_spec& default_color, const color_spec& color);
void textcolor_reset(FILE* f);
string textcolor_str(const color_spec& cs);
void textcolor_merge(const color_spec& cs1, color_spec* cs2);

string trim(const string& str);
void tokenize(const string& str, const string& delimiters, vector<string>* tokens);

//----------------------------------------------------------------
// main
//----------------------------------------------------------------

int main(int argc, char** argv) {
  assert (argv[argc] == NULL);

  technicolor_config config;
  char** child_argv;
  parse_command_line(argc, argv, &config, &child_argv);

  debug("child_argv[0] = " + string(child_argv[0]));

  int stdout_pipe[2];
  int stderr_pipe[2];

  assert (pipe(stdout_pipe) == 0);
  assert (pipe(stderr_pipe) == 0);

  // for child process
  pid_t childpid = fork();
  assert (childpid >= 0);

  if (childpid == 0) {
    // child process

    close(stdout_pipe[0]);
    close(stderr_pipe[0]);
    
    dup2(stdout_pipe[1], fileno(stdout));
    close(stdout_pipe[1]);
    
    dup2(stderr_pipe[1], fileno(stderr));
    close(stderr_pipe[1]);

    execvp(child_argv[0], child_argv);
    cerr << "technicolor: error executing command: " << child_argv[0] << endl;
    return 0;
  }

  // parent process
  close(stdout_pipe[1]);
  close(stderr_pipe[1]);

  string stdout_buffer;
  string stderr_buffer;

  bool stdout_closed = false;
  bool stderr_closed = false;

  while (true) {
    stdout_closed = read_bytes_available(stdout_pipe[0], stdout_buffer);
    stderr_closed = read_bytes_available(stderr_pipe[0], stderr_buffer);

    while (flush_complete_lines(config, stdout_buffer, stdout)) {;}
    while (flush_complete_lines(config, stderr_buffer, stderr)) {;}

    // final flush of anything in the buffers (not colorized because
    // there was no trailing newline).
    if (stdout_closed && stderr_closed) {
      if (stdout_buffer.length() > 0) { fprintf(stdout, stdout_buffer.c_str()); }
      if (stderr_buffer.length() > 0) { fprintf(stderr, stderr_buffer.c_str()); }
      debug("\nstdout and stderr both closed.  done!");
      break;
    }

    usleep(SLEEP_INTERVAL_USECS);
  }
}

//----------------------------------------------------------------
// parse_command_line
//----------------------------------------------------------------
void parse_command_line(int argc,
			char** argv,
			technicolor_config* config,
			char*** child_argv) {

  if (argc < 2) {
    cerr << USAGE << endl;
    exit(-1);
  }

  fill_default_config(config);

  //is argv[1] specifying a config file?
  if (string(argv[1]) == "--config") {
    if (argc < 4) {
      cerr << USAGE << endl;
      exit(-1);
    }
    load_config_file(argv[2], config);
    *child_argv = &argv[3];
  } else {
    *child_argv = &argv[1];
    char* config_dir = getenv("TECHNICOLOR_CONFIG_DIR");
    if (config_dir != NULL) {
      load_config_file((string(config_dir) + "/default"), config);
    }
  }
}

//----------------------------------------------------------------
// load_config_file
//----------------------------------------------------------------
void load_config_file(string filename, technicolor_config* config) {
  vector<string> file_lines;
  string line;
  file_lines.clear();
  ifstream infile(filename.c_str(), std::ios_base::in);
  if (!infile.good()) {
    cerr << "technicolor: Failed to open file " << filename << "." << endl;
    exit(-1);
  }

  while (getline(infile, line, '\n')) {
    file_lines.push_back(line);
  }
  infile.close();

  parse_config_file(file_lines, config);
}

//----------------------------------------------------------------
// fill_default_config
//  NOTE: just for testing.  Remove later.
//----------------------------------------------------------------
void fill_default_config(technicolor_config* config) {
  config->stdout_props.attr = RESET;
  config->stdout_props.foreground = WHITE;
  config->stdout_props.background = BLACK;

  config->stderr_props.attr = RESET;
  config->stderr_props.foreground = RED;
  config->stderr_props.background = BLACK;
}

//----------------------------------------------------------------
// parse_config_file
//----------------------------------------------------------------
void parse_config_file(const vector<string>& lines, technicolor_config* config) {
  // now parse all the lines into config
  for (vector<string>::const_iterator iter = lines.begin();
       iter != lines.end();
       iter++) {
    
    string line = *iter;
    
    // ignore blank lines or comments
    if ((line.length() < 1) or (line[0] == '#')) {
      continue;
    }

    parse_line_spec(line, config);
  }
}

//----------------------------------------------------------------
// parse_line_spec
//----------------------------------------------------------------
void parse_line_spec(const string& line, technicolor_config* config) {
  vector<string> side_tokens;
  tokenize(line, "=", &side_tokens);
  if (side_tokens.size() != 2) {
    cerr << "technicolor: Malformed config line:\n\t[" << line << "]" << endl;
    return;
  }
  string lhs = trim(side_tokens[0]);
  string rhs = trim(side_tokens[1]);

  line_spec ls;
  ls.regexp = boost::regex(lhs);  // lhs is just the regular expression
  ls.color_list.clear();
  
  // now go through rhs and push all the spec lists
  vector<string> tokens;
  tokenize(rhs, "()", &tokens);
  for (vector<string>::iterator iter = tokens.begin();
       iter != tokens.end();
       iter++) {
    
    string t = trim(*iter);
    if (t.length() < 1) {
      continue;
    }

    color_spec cs;
    if (!parse_color_spec(t, &cs)) {
      return;
    }

    ls.color_list.push_back(cs);
  }
  
  if (ls.color_list.size() < 1) {
    cerr << "technicolor: Malformed config line (has no color specs):\n\t"
	 << "[" << line << "]" << endl;
    return;
  }
  
  // add this line_spec to the config
  if (lhs == "<stdout>") {
    textcolor_merge(ls.color_list[0], &config->stdout_props);
  } else if (lhs == "<stderr>") {
    textcolor_merge(ls.color_list[0], &config->stderr_props);
  } else {
    config->spec_list.push_back(ls);
  }
}

//----------------------------------------------------------------
// parse_color_spec
//----------------------------------------------------------------
bool parse_color_spec(const string& str, color_spec* cs) {
  // str looks something like "fg:white bg:black attr:bright"

  cs->attr = -1;
  cs->foreground = -1;
  cs->background = -1;

  // first split line by spaces
  vector<string> tokens;
  tokenize(str, " ", &tokens);
  for (vector<string>::iterator iter = tokens.begin();
       iter != tokens.end();
       iter++) {

    string part = trim(*iter);
    if (part.length() < 1) {
      continue;
    }

    color_spec temp_spec;

    if (parse_color_spec_part(part, &temp_spec)) {
      textcolor_merge(temp_spec, cs);
    } else {
      cerr << "technicolor config error: failed to parse color spec ["
	   << str << "]" << endl;
      return false;  // nonsensical color spec part
    }
  }

  return true;
}

//----------------------------------------------------------------
// parse_color_spec_part
//----------------------------------------------------------------
bool parse_color_spec_part(const string& str, color_spec* cs) {
  // str looks something like "fg:white" or "attr:bright"

  vector<string> tokens;
  tokenize(str, ":", &tokens);

  if (tokens.size() != 2) {
    cerr << "technicolor config error: "
	 << "bad color spec part: [" << str << "]" << endl;
    return false;
  }

  cs->attr = -1;
  cs->foreground = -1;
  cs->background = -1;

  string part_str = trim(tokens[0]);
  string value_str = trim(tokens[1]);

  if (part_str == "attr") { cs->attr = lookup_attr_const(value_str); }
  else if (part_str == "bg") { cs->background = lookup_color_const(value_str); }
  else if (part_str == "fg") { cs->foreground = lookup_color_const(value_str); }
  else {
    cerr << "technicolor config error: "
	 << "unknown color part [" << part_str << "]" << endl;
    return false;
  }
  return true;
}

//----------------------------------------------------------------
// read_bytes_available
//----------------------------------------------------------------
bool read_bytes_available(int input_fd, string& line_buffer) {
  if (bytes_ready_to_read(input_fd)) {
    char buf[MAX_LINEBUFFER_SIZE];
    ssize_t bytes_read = read(input_fd, (void*)buf, MAX_LINEBUFFER_SIZE);
    stringstream ss; ss << "read " << bytes_read << " bytes.";
    debug(ss.str());
    if (bytes_read < 1) {
      // End of input file.
      return true;
    } else {
      buf[bytes_read] = '\0';
      line_buffer += buf;
    }
  } else {
    debug("no data available.");
  }
  return false;
}

//----------------------------------------------------------------
// flush_complete_lines
//----------------------------------------------------------------
bool flush_complete_lines(const technicolor_config& config,
			  string& buffer,
			  FILE* output_file) {
  if (buffer.length() < 1) {
    return false;
  }

  string::size_type i = buffer.find('\n');
  
  if (i == string::npos) {
    // no lines to flush
    return false;
  }

  string line = buffer.substr(0, i);
  buffer = buffer.substr(i+1, buffer.length());
  write_colored_line(config, line, output_file);
  return true;
}

//----------------------------------------------------------------
// write_colored_line
//----------------------------------------------------------------
void write_colored_line(const technicolor_config& config,
			const string& line,
			FILE* output_file) {

  color_spec default_color_spec =
    ((output_file == stderr) ? config.stderr_props : config.stdout_props);

  // see if this line matches any specs in specs
  for (vector<line_spec>::const_iterator iter = config.spec_list.begin();
       iter != config.spec_list.end();
       iter++) {
    line_spec spec = *iter;

    boost::smatch what;
    debug("testing string [" + line + "]");
    if (boost::regex_match(line, what, spec.regexp, boost::match_extra)) {
      debug("Found match! [" + line + "]");

      // see if this regular expression "covers" the whole line
      if (match_covers_line(line, spec.color_list, what)) {
	// proceed to output!
	assert ((what.size() - 1) == spec.color_list.size());
	for (int i = 0; i < spec.color_list.size(); ++i) {
	  textcolor(output_file, default_color_spec, spec.color_list[i]);
	  fprintf(output_file, string(what[i+1]).c_str());
	  textcolor_reset(output_file);
	}
	fprintf(output_file, "\n");
	return;
      } else {
	debug("match does not cover line =(");
      }
    }
  }

  // nothing matched... simply output the line
  textcolor(output_file, default_color_spec, default_color_spec);
  fprintf(output_file, line.c_str());
  fprintf(output_file, "\n");
  textcolor_reset(output_file);
}

//----------------------------------------------------------------
// match_covers_line
//----------------------------------------------------------------

bool match_covers_line(const string& line,
		       vector<color_spec> color_list,
		       const boost::smatch& what) {

  // TODO: actually implement this.
  return true;
}

//----------------------------------------------------------------
// bytes_ready_to_read
//----------------------------------------------------------------
bool bytes_ready_to_read(int fd) {
  fd_set read_fds;
  FD_ZERO(&read_fds);
  FD_SET(fd, &read_fds);
  int nfds = fd + 1;  // max(fds) + 1
  struct timeval timeout = { 0, SELECT_TIMEOUT_USECS };

  return select(nfds, &read_fds, NULL, NULL, &timeout);
}

//----------------------------------------------------------------
// debug
//----------------------------------------------------------------
void debug(string message) {
  if (DEBUG) {
    cout << message << endl;
  }
}

//----------------------------------------------------------------
// textcolor
//  Taken from [http://linuxgazette.net/issue65/padala.html]
//----------------------------------------------------------------
void textcolor(FILE* f, const color_spec& default_color, const color_spec& color) {
  char command[13];

  int attr = (color.attr == -1) ? default_color.attr : color.attr;
  int fg = (color.foreground == -1) ? default_color.foreground : color.foreground;
  int bg = (color.background == -1) ? default_color.background : color.background;

  assert (attr != -1);
  assert (fg != -1);
  assert (bg != -1);
  
  sprintf(command, "%c[%d;%d;%dm", 0x1B, attr, (fg + 30), (bg + 40));
  fprintf(f, command);
}

void textcolor_reset(FILE* f) {
  fprintf(f, "%c[0m", 0x1B);
}

string textcolor_str(const color_spec& cs) {
  stringstream oss;

  oss << "["
      << "attr:" << cs.attr << " "
      << "fg:" << cs.foreground << " "
      << "bg:" << cs.background
      << "]";

  return oss.str();
}

void textcolor_merge(const color_spec& cs1, color_spec* cs2) {

#define MERGE(X)       \
  if (cs1.X != -1) {   \
    cs2->X = cs1.X;    \
  }                    \

  MERGE(attr);
  MERGE(foreground);
  MERGE(background);
}

//----------------------------------------------------------------
// trimming strings
//  Taken from [http://www.thescripts.com/forum/thread63576.html]
//----------------------------------------------------------------
string trim(const string& str) {
  char* sepset = " \t\n\r";
  string::size_type first = str.find_first_not_of(sepset);
  if (first == string::npos) {
    return string();
  }
  string::size_type last = str.find_last_not_of(sepset);
  return str.substr(first, last - first + 1);
}

//----------------------------------------------------------------
// tokenizing strings
//  Taken from [http://www.digitalpeer.com/id/simple]
//----------------------------------------------------------------
void tokenize(const string& str, const string& delimiters, vector<string>* tokens) {
  tokens->clear();
  
  // skip delimiters at beginning.
  string::size_type lastPos = str.find_first_not_of(delimiters, 0);
  
  // find first "non-delimiter".
  string::size_type pos = str.find_first_of(delimiters, lastPos);
  
  while (string::npos != pos || string::npos != lastPos) {
    // found a token, add it to the vector.
    tokens->push_back(str.substr(lastPos, pos - lastPos));
		
    // skip delimiters.  Note the "not_of"
    lastPos = str.find_first_not_of(delimiters, pos);
    
    // find next "non-delimiter"
    pos = str.find_first_of(delimiters, lastPos);
  }
}
