/*** INCLUDES ***/

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/*** DEFINES ***/

#define KILO_VERSION "0.0.1"
#define KILO_TAB_STOP 8
#define KILO_QUIT_TIMES 3
#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKey {
	BACKSPACE = 127,
	ARROW_LEFT = 1000,
	ARROW_RIGHT,
	ARROW_UP,
	ARROW_DOWN,
	DEL_KEY,
	HOME_KEY,
	END_KEY,
	PAGE_UP,
	PAGE_DOWN
};

/*** DATA ***/

typedef struct erow {
	int size;
	int rsize;
	char *chars;
	char *render;
} erow;

struct editorConfig{
	int cx, cy;
	int rx;
	int rowoff;
	int coloff;
	int screenrows;
	int screencols;
	int numrows;
	erow *row;
	int dirty;
	char *filename;
	char statusmsg[80];
	time_t statusmsg_time;
	struct termios orig_termios;
};

struct editorConfig E;

/*** PROTOTYPES ***/

void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen();
char *editorPrompt(char *prompt);

/*** TERMINAL ****/

void die(const char *s){
	write(STDOUT_FILENO, "\x1b[2J", 4);
	write(STDOUT_FILENO, "\x1b[H", 3);
	perror(s);
	exit(1);
}

void disableRawMode(){
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1) 
		die("tcsetattr");
}

void enableRawMode(){
	if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1)
		die("tcgetattr");
	atexit(disableRawMode);

	struct termios raw = E.orig_termios;

	raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
	raw.c_oflag &= ~(OPOST);
	raw.c_cflag |= (CS8);
	raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
	raw.c_cc[VMIN] = 0;
	raw.c_cc[VTIME] = 1;
}

int editorReadKey(){
	int nread;
	char c;
	while ((nread = read(STDIN_FILENO, &c, 1)) != 1){
		if (nread == -1 && errno != EAGAIN) die("read");
	}

	if (c == '\x1b'){
		char seq[3];

		if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
		if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';
		if (seq[0] == '['){
			if (seq[1] >= '0' && seq[1] <= '9'){
				if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
				if (seq[2] == '~'){
					switch (seq[1]){
						case '1': return HOME_KEY;
						case '3': return DEL_KEY;
						case '4': return END_KEY;
						case '5': return PAGE_UP;
						case '6': return PAGE_DOWN;
						case '7': return HOME_KEY;
						case '8': return END_KEY;
					}
				}
			}
			else{
				switch (seq[1]){
					case 'A': return ARROW_UP;
					case 'B': return ARROW_DOWN;
					case 'C': return ARROW_RIGHT;
					case 'D': return ARROW_LEFT;
					case 'H': return HOME_KEY;
					case 'F': return END_KEY;
				}
			}
		}
		else if (seq[0] == 'O'){
			switch (seq[1]){
				case 'H': return HOME_KEY;
				case 'F': return END_KEY;
			}
		}
		return '\x1b';
	}
	else{
		return c;
	}
	return c;
}

int getCursorPosition(int *rows, int *cols){
	char buf[32];
	unsigned int i = 0;

	//queries the terminal for cursor position
	//returns Cursor Position Report as an escape sequence
	if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

	while (i < sizeof(buf) - 1){
		if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
		if (buf[i] == 'R') break;
		i++;
	}
	buf[i] = '\0';
	//check formatting of cursor position report
	if (buf[0] != '\x1b' || buf[1] != '[') return -1;
	//parse the x and y position out of the report
	if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

	return 0;
}

int getWindowSize(int *rows, int *cols){
	struct winsize ws;

	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0){
		//fallback window sizing if ioctl() can't request window size
		//C command moves cursor right, B command moves cursor down
		//Doesn't move past edge of screen, so just use 999 to guarantee we make it the whole way
		if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
		return getCursorPosition(rows, cols);
	}
	else{
		*cols = ws.ws_col;
		*rows = ws.ws_row;
		return 0;
	}
}

/*** ROW OPERATIONS ***/

//Convert cursor position in the raw string to cursor position in the rendered string
int editorRowCxToRx(erow *row, int cx){
	int rx = 0;
	int j;
	//Adjust for TABS
	for (j = 0; j < cx; j++){
		if(row->chars[j] == '\t')
			rx += (KILO_TAB_STOP - 1) - (rx % KILO_TAB_STOP);
		rx++;
	}
	return rx;
}

//Do operations on the raw text to get it into the state we want to actually render
void editorUpdateRow(erow *row){
	int tabs = 0;
	int j;
	//Allocate additional memory to render each tab character
	for (j = 0; j < row->size; j++)
		if (row->chars[j] == '\t') tabs++;
	free(row->render);
	row->render = malloc(row->size + tabs*(KILO_TAB_STOP - 1) + 1);

	int idx = 0;
	for (j = 0; j < row->size; j++){
		if(row->chars[j] == '\t'){
			//Add spaces until we get to a tabstop
			row->render[idx++] = ' ';
			while (idx % KILO_TAB_STOP != 0) row->render[idx++] = ' ';
		}
		else{
			row->render[idx++] = row->chars[j];
		}
	}
	row->render[idx] = '\0';
	row->rsize = idx;
}

void editorInsertRow(int at, char *s, size_t len){
	if (at < 0 || at > E.numrows) return;
	//move what's on the current row to the next one
	E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
	memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));
	//copy the chars of s into the erow
	E.row[at].size = len;
	E.row[at].chars = malloc(len+1);
	memcpy(E.row[at].chars, s, len);
	E.row[at].chars[len] = '\0';
	//update row for rendering
	E.row[at].rsize = 0;
	E.row[at].render = NULL;
	editorUpdateRow(&E.row[at]);

	E.numrows++;
	E.dirty++;
}

void editorFreeRow(erow *row){
	free(row->render);
	free(row->chars);
}

//remove memory for a row if we backspace at the beginning of a line
void editorDelRow(int at){
	if (at < 0 || at >= E.numrows) return;
	editorFreeRow(&E.row[at]);
	memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
	E.numrows--;
	E.dirty++;
}

//Insert character into the erow and allocate new memory
void editorRowInsertChar(erow *row, int at, int c){
	if (at < 0 || at > row->size) at = row->size;
	row->chars = realloc(row->chars, row->size + 2);
	memmove(&row->chars[at+1], &row->chars[at], row->size - at + 1);
	row->size++;
	row->chars[at] = c;
	editorUpdateRow(row);
	E.dirty++;
}

//Append multiple characters to the end of a row
void editorRowAppendString(erow *row, char *s, size_t len){
	row->chars = realloc(row->chars, row->size + len + 1);
	memcpy(&row->chars[row->size], s, len);
	row->size += len;
	row->chars[row->size] = '\0';
	editorUpdateRow(row);
	E.dirty++;
}

//Delete character and update the size of the erow
void editorRowDelChar(erow *row, int at){
	if (at < 0 || at >= row->size) return;
	memmove(&row->chars[at], &row->chars[at+1], row->size - at);
	row->size--;
	editorUpdateRow(row);
	E.dirty++;
}

/*** EDITOR OPERATIONS ***/

//Insert a character and worry about moving the cursor
void editorInsertChar(int c){
	if (E.cy == E.numrows){
		editorInsertRow(E.numrows, "", 0);
	}
	editorRowInsertChar(&E.row[E.cy], E.cx, c);
	E.cx++;
}

//
void editorInsertNewline(){
	//if we're at the beginning of the row, just insert a row where you are
	if (E.cx == 0){
		editorInsertRow(E.cy, "", 0);
	}
	//otherwise, move all the characters after the cursor to the new line when making it
	else{
		erow *row = &E.row[E.cy];
		editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
		//inserting calls realloc() which might move memory around, so reassign row
		row = &E.row[E.cy];
		row->size = E.cx;
		row->chars[row->size] = '\0';
		editorUpdateRow(row);
	}
	E.cy++;
	E.cx = 0;
}

//Call the row operation for deletion and move the cursor
void editorDelChar(){
	//dont do anything if past the end of the file
	if (E.cy == E.numrows) return;
	//dont do anything if we're at the beginning of the file
	if (E.cx == 0 && E.cy == 0) return;

	erow *row = &E.row[E.cy];
	//delete character if not on the first character of the row
	if (E.cx > 0){
		editorRowDelChar(row, E.cx - 1);
		E.cx--;
	}
	//delete row if on the first character of the row
	else {
		E.cx = E.row[E.cy - 1].size;
		editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
		editorDelRow(E.cy);
		E.cy--;
	}
}

/*** FILE I/0 ***/

char *editorRowsToString(int *buflen){
	//totlen is the total number of characters in the file
	int totlen = 0;
	int j;
	for (j = 0; j < E.numrows; j++)
		totlen += E.row[j].size + 1;
	*buflen = totlen;
	//allocate enough memory for the entire file
	char* buf = malloc(totlen);
	char *p = buf;
	//copy each line, move the pointer and add a newline
	for (j = 0; j < E.numrows; j++){
		memcpy(p, E.row[j].chars, E.row[j].size);
		p += E.row[j].size;
		*p = '\n';
		p++;
	}
	//expects caller to free the memory used by buf
	return buf;
}

//Open and read a file from disc. Only called if program run supplied with args
void editorOpen(char *filename){
	//Add the input file name to the editorConfig
	free(E.filename);
	E.filename = strdup(filename);
	//Look for file with provided filename
	FILE *fp = fopen(filename, "r");
	if (!fp) die("fopen");

	char *line = NULL;
	size_t linecap = 0;
	ssize_t linelen;
	//Look at each line and copy its contents to our editorConfig struct 
	while ((linelen = getline(&line, &linecap, fp)) != -1){
		while(linelen > 0 && (line[linelen-1] == '\n'|| line[linelen-1] == '\r'))
			linelen--;
		editorInsertRow(E.numrows, line, linelen);
	}
	free(line);
	fclose(fp);
	E.dirty = 0;
}

void editorSave(){
	if (E.filename == NULL){
		E.filename = editorPrompt("Save as: %s (ESC to cancel)");
		if (E.filename == NULL){
			editorSetStatusMessage("Save aborted");
			return;
		}
	}

	int len;
	char *buf = editorRowsToString(&len);
	//create if doesnt exist, read/write, 0644 is standard text file permissions
	int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
	if (fd != -1) {
		if (ftruncate(fd, len) != -1){
			if(write(fd, buf, len) == len){
				close(fd);
				free(buf);
				E.dirty = 0;
				editorSetStatusMessage("%d bytes written to disk", len);
				return;
			}
		}
		close(fd);
	}
	free(buf);
	editorSetStatusMessage("Can't save! I/0 error: %s", strerror(errno));
}

/*** APPEND BUFFER ***/

//C doesn't have dynamic strings, so we're just gonna make it ourselves

struct abuf {
	char *b;
	int len;
};

#define ABUF_INIT {NULL, 0}

//append a new string to the existing buffer
void abAppend(struct abuf *ab, const char *s, int len){
	//allocate new space for what we're appending
	char *new = realloc(ab->b, ab->len + len);

	if(new == NULL) return;
	//concat the existing string and the new string 's'
	memcpy(&new[ab->len], s, len);
	ab->b = new;
	ab->len += len;
}

//deallocate the memory used by ab
void abFree(struct abuf *ab){
	free(ab->b);
}

/*** OUTPUT ***/

void editorScroll(){
	//Get length of rendered row
	E.rx = 0;
	if(E.cy < E.numrows){
		E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
	}

	if (E.cy < E.rowoff){
		E.rowoff = E.cy;
	}
	if (E.cy >= E.rowoff + E.screenrows) {
		E.rowoff = E.cy - E.screenrows + 1;
	}
	if (E.rx < E.coloff){
		E.coloff = E.rx;
	}
	if (E.rx >= E.coloff + E.screencols){
		E.coloff = E.rx - E.screencols + 1;
	}
}

void editorDrawRows(struct abuf *ab){
	int y;
	for (y = 0; y < E.screenrows; y++) {
		int filerow = y + E.rowoff;
		if (filerow >= E.numrows){
			//Create welcome message a third of the way down the screen
			//Only if there isn't a file being loaded
			if (E.numrows == 0 && y == E.screenrows / 3){
				char welcome[80];
				int welcomelen = snprintf(welcome, sizeof(welcome),
					"Kilo editor -- version %s", KILO_VERSION);
				if (welcomelen > E.screencols) welcomelen = E.screencols;
				//Append padding to center the message in the middle of the screen
				int padding = (E.screencols - welcomelen) / 2;
				if (padding){
					abAppend(ab, "~", 1);
					padding--;
				}
				while (padding--) abAppend(ab, " ", 1);
				abAppend(ab, welcome, welcomelen);
			}
			else{
				abAppend(ab, "~", 1);
			}
		}
		else{
			int len = E.row[filerow].rsize - E.coloff;
			if (len < 0) len = 0;
			if (len > E.screencols) len = E.screencols;
			abAppend(ab, &E.row[filerow].render[E.coloff], len);
		}
		//erase what's currently in each line as we draw the rows
		abAppend(ab, "\x1b[K", 3);
		abAppend(ab, "\r\n", 2);			
	}
}

void editorDrawStatusBar(struct abuf *ab){
	//Switches to inverted color formatting
	abAppend(ab, "\x1b[7m", 4);
	//row status, NOT render status lol
	char status[80], rstatus[80];
	//Print the filename or a default if there isn't a file
	int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
		E.filename ? E.filename : "[No Name]", E.numrows,
		E.dirty ? "(modified)" : "");
	int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d",
		E.cy + 1, E.numrows);
	if (len > E.screencols) len = E.screencols;
	abAppend(ab, status, len);
	//Fill with empty spaces
	while (len < E.screencols){
		if (E.screencols - len == rlen){
			abAppend(ab, rstatus, rlen);
			break;
		}
		else{
			abAppend(ab, " ", 1);
			len++;
		}
	}
	abAppend(ab, "\x1b[m", 3);
	abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct abuf *ab){
	abAppend(ab, "\x1b[K", 3);
	int msglen = strlen(E.statusmsg);
	if (msglen > E.screencols) msglen = E.screencols;
	if (msglen && time(NULL) - E.statusmsg_time < 5)
		abAppend(ab, E.statusmsg, msglen);
}

void editorRefreshScreen(){
	//Scroll the text if the cursor is offscreen
	editorScroll();

	struct abuf ab = ABUF_INIT;
	//Hide the cursor while we're repainting the terminal
	abAppend(&ab, "\x1b[?25l", 6);
	//H repositions the cursor, default is (1, 1) which is what we want
	abAppend(&ab, "\x1b[H", 3);

	editorDrawRows(&ab);
	editorDrawStatusBar(&ab);
	editorDrawMessageBar(&ab);

	//Move the cursor to the current stored position
	char buf[32];
	snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1, (E.rx - E.coloff) + 1);
	abAppend(&ab, buf, strlen(buf));

	abAppend(&ab, "\x1b[?25h", 6);

	write(STDOUT_FILENO, ab.b, ab.len);
	abFree(&ab);
}

void editorSetStatusMessage(const char *fmt, ...){
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
	va_end(ap);
	E.statusmsg_time = time(NULL);
}

/*** INPUT ***/

char *editorPrompt(char *prompt){
	size_t bufsize = 128;
	char *buf = malloc(bufsize);

	size_t buflen = 0;
	buf[0] = '\0';

	while(1){
		editorSetStatusMessage(prompt, buf);
		editorRefreshScreen();

		int c = editorReadKey();
		if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE){
			if (buflen != 0) buf[--buflen] = '\0';
		}
		else if (c == '\x1b'){
			editorSetStatusMessage("");
			free(buf);
			return NULL;
		}
		else if (c == '\r'){
			if (buflen != 0){
				editorSetStatusMessage("");
				return buf;
			}
		}
		else if (!iscntrl(c) && c < 128){
			if (buflen == bufsize - 1){
				bufsize *= 2;
			}
			buf[buflen++] = c;
			buf[buflen] = '\0';
		}
	}
}

void editorMoveCursor(int key){
	//check if the cursor is on an actual line
	erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

	//Use WASD to move the cursor
	switch (key) {
		case ARROW_LEFT:
			if(E.cx != 0){
				E.cx--;
			}
			//If at beginning of row, move to the end of the row above this one
			else if (E.cy > 0){
				E.cy--;
				E.cx = E.row[E.cy].size;
			}
			break;
		case ARROW_RIGHT:
			//check if cursor is to the left of the end of the current line
			if (row && E.cx < row->size){
				E.cx++;
			}
			else if (row && E.cx == row->size){
				E.cy++;
				E.cx = 0;
			}
			break;
		case ARROW_UP:
			if(E.cy != 0){
				E.cy--;
			}
			break;
		case ARROW_DOWN:
			if(E.cy < E.numrows){
				E.cy++;
			}
			break;
	}

	//Correct x pos if past the end of the current row
	row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
	int rowlen = row ? row->size : 0;
	if (E.cx > rowlen){
		E.cx = rowlen;
	}
}

void editorProcessKeypress(){
	static int quit_times = KILO_QUIT_TIMES;

	int c = editorReadKey();

	switch (c) {
		case '\r':
			editorInsertNewline();
			break;
		case CTRL_KEY('q'):
			if (E.dirty && quit_times > 0){
				editorSetStatusMessage("WARNING!!! File has unsaved changes! Press Ctrl-Q %d more times to quit.", quit_times);
				quit_times--;
				return;
			}
			write(STDOUT_FILENO, "\x1b[2J", 4);
			write(STDOUT_FILENO, "\x1b[H", 3);
			exit(0);
			break;
		case CTRL_KEY('s'):
			editorSave();
			break;
		// Home and End move the cursor to the left or right end of the current row
		case HOME_KEY:
			E.cx = 0;
			break;
		case END_KEY:
			if (E.cy < E.numrows) E.cx = E.row[E.cy].size;	
			break;
		//various deletion keys
		case BACKSPACE:
		case CTRL_KEY('h'):
		case DEL_KEY:
			if (c == DEL_KEY) editorMoveCursor(ARROW_RIGHT);
			editorDelChar();
			break;
		//Page up and down simulate arrow presses to scroll an entire page
		case PAGE_UP:
		case PAGE_DOWN:
			{
				if (c == PAGE_UP){
					E.cy = E.rowoff;
				}
				else if (c == PAGE_DOWN){
					E.cy = E.rowoff + E.screenrows - 1;
					if (E.cy > E.numrows) E.cy = E.numrows;
				}
				int times = E.screenrows;
				while (times--)
					editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
			}
			break;

		case ARROW_UP:
		case ARROW_DOWN:
		case ARROW_LEFT:
		case ARROW_RIGHT:
			editorMoveCursor(c);
			break;
		case CTRL_KEY('l'):
		case '\x1b':
			break;
		default:
			editorInsertChar(c);
			break;
	}

	quit_times = KILO_QUIT_TIMES;
}

/*** INIT ***/

void initEditor(){
	//Initialize all editorConfig variables
	E.cx = 0;
	E.cy = 0;
	E.rx = 0;
	E.rowoff = 0;
	E.coloff = 0;
	E.numrows = 0;
	E.row = NULL;
	E.dirty = 0;
	E.filename = NULL;
	E.statusmsg[0] = '\0';
	E.statusmsg_time = 0;

	if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
	//Make room for status bar
	E.screenrows -= 2;
}

int main(int argc, char *argv[]){
	enableRawMode();
	initEditor();
	if (argc >= 2){
		editorOpen(argv[1]);
	}

	editorSetStatusMessage("HELP: Ctrl-S = save | Ctrl-Q = quit");

	while (1){
		editorRefreshScreen();
		editorProcessKeypress();
	}
	return 0;
}
