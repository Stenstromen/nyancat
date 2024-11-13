/*
 * Copyright (c) 2011-2018 K. Lange.  All rights reserved.
 *
 * Developed by:            K. Lange
 *                          http://github.com/klange/nyancat
 *                          http://nyancat.dakko.us
 *
 * 40-column support by:    Peter Hazenberg
 *                          http://github.com/Peetz0r/nyancat
 *                          http://peter.haas-en-berg.nl
 *
 * Build tools unified by:  Aaron Peschel
 *                          https://github.com/apeschel
 *
 * For a complete listing of contributors, please see the git commit history.
 *
 * This is a simple telnet server / standalone application which renders the
 * classic Nyan Cat (or "poptart cat") to your terminal.
 *
 * It makes use of various ANSI escape sequences to render color, or in the case
 * of a VT220, simply dumps text to the screen.
 *
 * For more information, please see:
 *
 *     http://nyancat.dakko.us
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal with the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *   1. Redistributions of source code must retain the above copyright notice,
 *      this list of conditions and the following disclaimers.
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimers in the
 *      documentation and/or other materials provided with the distribution.
 *   3. Neither the names of the Association for Computing Machinery, K.
 *      Lange, nor the names of its contributors may be used to endorse
 *      or promote products derived from this Software without specific prior
 *      written permission.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
 * CONTRIBUTORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * WITH THE SOFTWARE.
 */

#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE 1
#define _BSD_SOURCE
#define _DEFAULT_SOURCE
#define __BSD_VISIBLE 1
#include <ctype.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <setjmp.h>
#include <getopt.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <sys/ioctl.h>

#ifndef TIOCGWINSZ
#include <termios.h>
#endif

#ifdef ECHO
#undef ECHO
#endif

/*
 * telnet.h contains some #defines for the various
 * commands, escape characters, and modes for telnet.
 * (it surprises some people that telnet is, really,
 *  a protocol, and not just raw text transmission)
 */
#include "telnet.h"

/*
 * The animation frames are stored separately in
 * this header so they don't clutter the core source
 */
#include "animation.c"

/*
 * Color palette to use for final output
 * Specifically, this should be either control sequences
 * or raw characters (ie, for vt220 mode)
 */
const char * colors[256] = {NULL};

/*
 * For most modes, we output spaces, but for some
 * we will use block characters (or even nothing)
 */
const char * output = "  ";

/* Global variables */
int telnet = 0;
int show_counter = 1;
int always_escape = 0;
int delay_ms = 90;

/*
 * Number of frames to show before quitting
 * or 0 to repeat forever (default)
 */
unsigned int frame_count = 0;

/*
 * Clear the screen between frames (as opposed to resetting
 * the cursor position)
 */
int clear_screen = 1;

/*
 * Force-set the terminal title.
 */
int set_title = 1;

/*
 * Environment to use for setjmp/longjmp
 * when breaking out of options handler
 */
jmp_buf environment;

/* Default telnet port */
int server_port = 23;
/* Listen mode flag */
int listen_mode = 0;

/* Add these with the other external declarations */
extern int skip_intro;
extern int show_intro;

/*
 * I refuse to include libm to keep this low
 * on external dependencies.
 *
 * Count the number of digits in a number for
 * use with string output.
 */
int digits(int val) {
	int d = 1, c;
	if (val >= 0) for (c = 10; c <= val; c *= 10) d++;
	else for (c = -10 ; c >= val; c *= 10) d++;
	return (c < 0) ? ++d : d;
}

/*
 * These values crop the animation, as we have a full 64x64 stored,
 * but we only want to display 40x24 (double width).
 */
int min_row = -1;
int max_row = -1;
int min_col = -1;
int max_col = -1;

/*
 * Actual width/height of terminal.
 */
int terminal_width = 80;
int terminal_height = 24;

/*
 * Flags to keep track of whether width/height were automatically set.
 */
char using_automatic_width = 0;
char using_automatic_height = 0;

/*
 * Print escape sequences to return cursor to visible mode
 * and exit the application.
 */
void finish(void) {
	if (clear_screen) {
		printf("\033[?25h\033[0m\033[H\033[2J");
	} else {
		printf("\033[0m\n");
	}
	exit(0);
}

/*
 * In the standalone mode, we want to handle an interrupt signal
 * (^C) so that we can restore the cursor and clear the terminal.
 */
void SIGINT_handler(int sig){
	(void)sig;
	finish();
}

/*
 * Handle the alarm which breaks us off of options
 * handling if we didn't receive a terminal
 */
void SIGALRM_handler(int sig) {
	(void)sig;
	alarm(0);
	longjmp(environment, 1);
	/* Unreachable */
}

/*
 * Handle the loss of stdout, as would be the case when
 * in telnet mode and the client disconnects
 */
void SIGPIPE_handler(int sig) {
	(void)sig;
	finish();
}

void SIGWINCH_handler(int sig) {
	(void)sig;
	struct winsize w;
	ioctl(0, TIOCGWINSZ, &w);
	terminal_width = w.ws_col;
	terminal_height = w.ws_row;

	if (using_automatic_width) {
		min_col = (FRAME_WIDTH - terminal_width/2) / 2;
		max_col = (FRAME_WIDTH + terminal_width/2) / 2;
	}

	if (using_automatic_height) {
		min_row = (FRAME_HEIGHT - (terminal_height-1)) / 2;
		max_row = (FRAME_HEIGHT + (terminal_height-1)) / 2;
	}

	signal(SIGWINCH, SIGWINCH_handler);
}

/*
 * Telnet requires us to send a specific sequence
 * for a line break (\r\000\n), so let's make it happy.
 */
void newline(int n) {
	int i = 0;
	for (i = 0; i < n; ++i) {
		/* We will send `n` linefeeds to the client */
		if (telnet) {
			/* Send the telnet newline sequence */
			putc('\r', stdout);
			putc(0, stdout);
			putc('\n', stdout);
		} else {
			/* Send a regular line feed */
			putc('\n', stdout);
		}
	}
}

/*
 * These are the options we want to use as
 * a telnet server. These are set in set_options()
 */
unsigned char telnet_options[256] = { 0 };
unsigned char telnet_willack[256] = { 0 };

/*
 * These are the values we have set or
 * agreed to during our handshake.
 * These are set in send_command(...)
 */
unsigned char telnet_do_set[256]  = { 0 };
unsigned char telnet_will_set[256]= { 0 };

/*
 * Set the default options for the telnet server.
 */
void set_options(void) {
	/* We will not echo input */
	telnet_options[ECHO] = WONT;
	/* We will set graphics modes */
	telnet_options[SGA]  = WILL;
	/* We will not set new environments */
	telnet_options[NEW_ENVIRON] = WONT;

	/* The client should echo its own input */
	telnet_willack[ECHO]  = DO;
	/* The client can set a graphics mode */
	telnet_willack[SGA]   = DO;
	/* The client should not change, but it should tell us its window size */
	telnet_willack[NAWS]  = DO;
	/* The client should tell us its terminal type (very important) */
	telnet_willack[TTYPE] = DO;
	/* No linemode */
	telnet_willack[LINEMODE] = DONT;
	/* And the client can set a new environment */
	telnet_willack[NEW_ENVIRON] = DO;
}

/*
 * Send a command (cmd) to the telnet client
 * Also does special handling for DO/DONT/WILL/WONT
 */
void send_command(int cmd, int opt) {
	/* Send a command to the telnet client */
	if (cmd == DO || cmd == DONT) {
		/* DO commands say what the client should do. */
		if (((cmd == DO) && (telnet_do_set[opt] != DO)) ||
			((cmd == DONT) && (telnet_do_set[opt] != DONT))) {
			/* And we only send them if there is a disagreement */
			telnet_do_set[opt] = cmd;
			printf("%c%c%c", IAC, cmd, opt);
		}
	} else if (cmd == WILL || cmd == WONT) {
		/* Similarly, WILL commands say what the server will do. */
		if (((cmd == WILL) && (telnet_will_set[opt] != WILL)) ||
			((cmd == WONT) && (telnet_will_set[opt] != WONT))) {
			/* And we only send them during disagreements */
			telnet_will_set[opt] = cmd;
			printf("%c%c%c", IAC, cmd, opt);
		}
	} else {
		/* Other commands are sent raw */
		printf("%c%c", IAC, cmd);
	}
}

/*
 * Print the usage / help text describing options
 */
void usage(char * argv[]) {
	printf(
			"Terminal Nyancat\n"
			"\n"
			"usage: %s [-hitn] [-f \033[3mframes\033[0m]\n"
			"\n"
			" -i --intro      \033[3mShow the introduction / about information at startup.\033[0m\n"
			" -t --telnet     \033[3mTelnet mode.\033[0m\n"
			" -n --no-counter \033[3mDo not display the timer\033[0m\n"
			" -s --no-title   \033[3mDo not set the titlebar text\033[0m\n"
			" -e --no-clear   \033[3mDo not clear the display between frames\033[0m\n"
			" -d --delay      \033[3mDelay image rendering by anywhere between 10ms and 1000ms\n"
			" -f --frames     \033[3mDisplay the requested number of frames, then quit\033[0m\n"
			" -r --min-rows   \033[3mCrop the animation from the top\033[0m\n"
			" -R --max-rows   \033[3mCrop the animation from the bottom\033[0m\n"
			" -c --min-cols   \033[3mCrop the animation from the left\033[0m\n"
			" -C --max-cols   \033[3mCrop the animation from the right\033[0m\n"
			" -W --width      \033[3mCrop the animation to the given width\033[0m\n"
			" -H --height     \033[3mCrop the animation to the given height\033[0m\n"
			" -h --help       \033[3mShow this help message.\033[0m\n",
			argv[0]);
}


void run_telnet_server(void) {
    int server_fd;
    struct sockaddr_in server_addr;
    
    /* Create socket */
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("Could not create socket");
        exit(1);
    }
    
    /* Allow port reuse */
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt failed");
        exit(1);
    }
    
    /* Setup server address structure */
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(server_port);
    
    /* Bind */
    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        exit(1);
    }
    
    /* Listen */
    if (listen(server_fd, 5) < 0) {
        perror("Listen failed");
        exit(1);
    }
    
    printf("Nyancat telnet server listening on port %d\n", server_port);
    
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        /* Accept connection */
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            perror("Accept failed");
            continue;
        }
        
        /* Fork to handle client */
        pid_t pid = fork();
        if (pid < 0) {
            perror("Fork failed");
            close(client_fd);
            continue;
        }
        
        if (pid == 0) {
            /* Child process */
            close(server_fd);
            
            /* Redirect stdout to client socket */
            dup2(client_fd, STDOUT_FILENO);
            dup2(client_fd, STDIN_FILENO);
            
            /* Set telnet mode */
            telnet = 1;
            
            /* Run main animation loop */
            set_options();
            
            /* Let the client know what we're using */
            for (int option = 0; option < 256; option++) {
                if (telnet_options[option]) {
                    send_command(telnet_options[option], option);
                    fflush(stdout);
                }
            }
            for (int option = 0; option < 256; option++) {
                if (telnet_willack[option]) {
                    send_command(telnet_willack[option], option);
                    fflush(stdout);
                }
            }
            
            /* Store the start time */
            time_t start, current;
            time(&start);

            int playing = 1;    /* Animation should continue */
            size_t i = 0;       /* Current frame # */
            unsigned int f = 0;  /* Total frames passed */
            char last = 0;      /* Last color index rendered */
            int y, x;           /* x/y coordinates of what we're drawing */

            /* Main animation loop from lines 974-1060 */
            while (playing) {
                if (clear_screen) {
                    printf("\033[H");
                } else {
                    printf("\033[u");
                }
                
                for (y = min_row; y < max_row; ++y) {
                    for (x = min_col; x < max_col; ++x) {
                        char color;
                        if (y > 23 && y < 43 && x < 0) {
                            int mod_x = ((-x+2) % 16) / 8;
                            if ((i / 2) % 2) {
                                mod_x = 1 - mod_x;
                            }
                            const char *rainbow = ",,>>&&&+++###==;;;,,";
                            color = rainbow[mod_x + y-23];
                            if (color == 0) color = ',';
                        } else if (x < 0 || y < 0 || y >= FRAME_HEIGHT || x >= FRAME_WIDTH) {
                            color = ',';
                        } else {
                            color = frames[i][y][x];
                        }
                        if (always_escape) {
                            printf("%s", colors[(int)color]);
                        } else {
                            if (color != last && colors[(int)color]) {
                                last = color;
                                printf("%s%s", colors[(int)color], output);
                            } else {
                                printf("%s", output);
                            }
                        }
                    }
                    newline(1);
                }

                if (show_counter) {
                    time(&current);
                    double diff = difftime(current, start);
                    int nLen = digits((int)diff);
                    int width = (terminal_width - 29 - nLen) / 2;
                    while (width > 0) {
                        printf(" ");
                        width--;
                    }
                    printf("\033[1;37mYou have nyaned for %0.0f seconds!\033[J\033[0m", diff);
                }

                last = 0;
                ++f;
                if (frame_count != 0 && f == frame_count) {
                    break;
                }
                ++i;
                if (!frames[i]) {
                    i = 0;
                }
                usleep(1000 * delay_ms);
            }
            
            close(client_fd);
            exit(0);
        } else {
            /* Parent process */
            close(client_fd);
        }
    }
}

int main(int argc, char ** argv) {

	char *term = NULL;
	unsigned int k;
	int ttype;
	uint32_t option = 0, done = 0, sb_mode = 0;
	/* Various pieces for the telnet communication */
	unsigned char  sb[1024] = {0};
	unsigned short sb_len   = 0;

	/* Whether or not to show the MOTD intro */
	int show_intro = 0;
	int skip_intro = 0;

	/* Long option names */
	static struct option long_opts[] = {
		{"help",       no_argument,       0, 'h'},
		{"telnet",     no_argument,       0, 't'},
		{"intro",      no_argument,       0, 'i'},
		{"skip-intro", no_argument,       0, 'I'},
		{"no-counter", no_argument,       0, 'n'},
		{"no-title",   no_argument,       0, 's'},
		{"no-clear",   no_argument,       0, 'e'},
		{"delay",      required_argument, 0, 'd'},
		{"frames",     required_argument, 0, 'f'},
		{"min-rows",   required_argument, 0, 'r'},
		{"max-rows",   required_argument, 0, 'R'},
		{"min-cols",   required_argument, 0, 'c'},
		{"max-cols",   required_argument, 0, 'C'},
		{"width",      required_argument, 0, 'W'},
		{"height",     required_argument, 0, 'H'},
		{"listen",     no_argument,       0, 'l'},
		{"port",       required_argument, 0, 'p'},
		{0,0,0,0}
	};

	/* Time delay in milliseconds */
	delay_ms = 90; // Default to original value

	/* Process arguments */
	int index, c;
	while ((c = getopt_long(argc, argv, "eshiItnd:f:r:R:c:C:W:H:lp:", long_opts, &index)) != -1) {
		if (!c) {
			if (long_opts[index].flag == 0) {
				c = long_opts[index].val;
			}
		}
		switch (c) {
			case 'e':
				clear_screen = 0;
				break;
			case 's':
				set_title = 0;
				break;
			case 'i': /* Show introduction */
				show_intro = 1;
				break;
			case 'I':
				skip_intro = 1;
				break;
			case 't': /* Expect telnet bits */
				telnet = 1;
				break;
			case 'h': /* Show help and exit */
				usage(argv);
				exit(0);
				break;
			case 'n':
				show_counter = 0;
				break;
 			case 'd':
				if (10 <= atoi(optarg) && atoi(optarg) <= 1000)
					delay_ms = atoi(optarg);
				break;
			case 'f':
				frame_count = atoi(optarg);
				break;
			case 'r':
				min_row = atoi(optarg);
				break;
			case 'R':
				max_row = atoi(optarg);
				break;
			case 'c':
				min_col = atoi(optarg);
				break;
			case 'C':
				max_col = atoi(optarg);
				break;
			case 'W':
				min_col = (FRAME_WIDTH - atoi(optarg)) / 2;
				max_col = (FRAME_WIDTH + atoi(optarg)) / 2;
				break;
			case 'H':
				min_row = (FRAME_HEIGHT - atoi(optarg)) / 2;
				max_row = (FRAME_HEIGHT + atoi(optarg)) / 2;
				break;
			case 'l':
				listen_mode = 1;
				break;
			case 'p':
				server_port = atoi(optarg);
				if (server_port <= 0 || server_port > 65535) {
					fprintf(stderr, "Invalid port number\n");
					return 1;
				}
				break;
			default:
				break;
		}
	}

	if (telnet) {
		/* Telnet mode */

		/* show_intro is implied unless skip_intro was set */
		show_intro = (skip_intro == 0) ? 1 : 0;

		/* Set the default options */
		set_options();

		/* Let the client know what we're using */
		for (option = 0; option < 256; option++) {
			if (telnet_options[option]) {
				send_command(telnet_options[option], option);
				fflush(stdout);
			}
		}
		for (option = 0; option < 256; option++) {
			if (telnet_willack[option]) {
				send_command(telnet_willack[option], option);
				fflush(stdout);
			}
		}

		/* Set the alarm handler to execute the longjmp */
		signal(SIGALRM, SIGALRM_handler);

		/* Negotiate options */
		if (!setjmp(environment)) {
			/* We will stop handling options after one second */
			alarm(1);

			/* Let's do this */
			while (!feof(stdin) && done < 2) {
				/* Get either IAC (start command) or a regular character (break, unless in SB mode) */
				unsigned char i = getchar();
				unsigned char opt = 0;
				if (i == IAC) {
					/* If IAC, get the command */
					i = getchar();
					switch (i) {
						case SE:
							/* End of extended option mode */
							sb_mode = 0;
							if (sb[0] == TTYPE) {
								/* This was a response to the TTYPE command, meaning
								 * that this should be a terminal type */
								alarm(2);
								term = strndup((char *)&sb[2], sizeof(sb)-2);
								done++;
							}
							else if (sb[0] == NAWS) {
								/* This was a response to the NAWS command, meaning
								 * that this should be a window size */
								alarm(2);
								terminal_width = (sb[1] << 8) | sb[2];
								terminal_height = (sb[3] << 8) | sb[4];
								done++;
							}
							break;
						case NOP:
							/* No Op */
							send_command(NOP, 0);
							fflush(stdout);
							break;
						case WILL:
						case WONT:
							/* Will / Won't Negotiation */
							opt = getchar();
							if (!telnet_willack[opt]) {
								/* We default to WONT */
								telnet_willack[opt] = WONT;
							}
							send_command(telnet_willack[opt], opt);
							fflush(stdout);
							if ((i == WILL) && (opt == TTYPE)) {
								/* WILL TTYPE? Great, let's do that now! */
								printf("%c%c%c%c%c%c", IAC, SB, TTYPE, SEND, IAC, SE);
								fflush(stdout);
							}
							break;
						case DO:
						case DONT:
							/* Do / Don't Negotiation */
							opt = getchar();
							if (!telnet_options[opt]) {
								/* We default to DONT */
								telnet_options[opt] = DONT;
							}
							send_command(telnet_options[opt], opt);
							fflush(stdout);
							break;
						case SB:
							/* Begin Extended Option Mode */
							sb_mode = 1;
							sb_len  = 0;
							memset(sb, 0, sizeof(sb));
							break;
						case IAC: 
							/* IAC IAC? That's probably not right. */
							done = 2;
							break;
						default:
							break;
					}
				} else if (sb_mode) {
					/* Extended Option Mode -> Accept character */
					if (sb_len < sizeof(sb) - 1) {
						/* Append this character to the SB string,
						 * but only if it doesn't put us over
						 * our limit; honestly, we shouldn't hit
						 * the limit, as we're only collecting characters
						 * for a terminal type or window size, but better safe than
						 * sorry (and vulnerable).
						 */
						sb[sb_len] = i;
						sb_len++;
					}
				}
			}
		}
		alarm(0);
	} else {
		/* We are running standalone, retrieve the
		 * terminal type from the environment. */
		term = getenv("TERM");

		/* Also get the number of columns */
		struct winsize w;
		ioctl(0, TIOCGWINSZ, &w);
		terminal_width = w.ws_col;
		terminal_height = w.ws_row;
	}

	/* Default ttype */
	ttype = 2;

	if (term) {
		/* Convert the entire terminal string to lower case */
		for (k = 0; k < strlen(term); ++k) {
			term[k] = tolower(term[k]);
		}

		/* Do our terminal detection */
		if (strstr(term, "xterm")) {
			ttype = 1; /* 256-color, spaces */
		} else if (strstr(term, "toaru")) {
			ttype = 1; /* emulates xterm */
		} else if (strstr(term, "linux")) {
			ttype = 3; /* Spaces and blink attribute */
		} else if (strstr(term, "vtnt")) {
			ttype = 5; /* Extended ASCII fallback == Windows */
		} else if (strstr(term, "cygwin")) {
			ttype = 5; /* Extended ASCII fallback == Windows */
		} else if (strstr(term, "vt220")) {
			ttype = 6; /* No color support */
		} else if (strstr(term, "fallback")) {
			ttype = 4; /* Unicode fallback */
		} else if (strstr(term, "rxvt-256color")) {
			ttype = 1; /* xterm 256-color compatible */
		} else if (strstr(term, "rxvt")) {
			ttype = 3; /* Accepts LINUX mode */
		} else if (strstr(term, "vt100") && terminal_width == 40) {
			ttype = 7; /* No color support, only 40 columns */
		} else if (!strncmp(term, "st", 2)) {
			ttype = 1; /* suckless simple terminal is xterm-256color-compatible */
		} else if (!strncmp(term, "truecolor", 9)) {
			ttype = 8;
		}
	}

	/* Accept ^C -> restore cursor */
	signal(SIGINT, SIGINT_handler);

	/* Handle loss of stdout */
	signal(SIGPIPE, SIGPIPE_handler);

	/* Handle window changes */
	if (!telnet) {
		signal(SIGWINCH, SIGWINCH_handler);
	}

	switch (ttype) {
		case 1:
			colors[',']  = "\033[48;5;17m";  /* Blue background */
			colors['.']  = "\033[48;5;231m"; /* White stars */
			colors['\''] = "\033[48;5;16m";  /* Black border */
			colors['@']  = "\033[48;5;230m"; /* Tan poptart */
			colors['$']  = "\033[48;5;175m"; /* Pink poptart */
			colors['-']  = "\033[48;5;162m"; /* Red poptart */
			colors['>']  = "\033[48;5;196m"; /* Red rainbow */
			colors['&']  = "\033[48;5;214m"; /* Orange rainbow */
			colors['+']  = "\033[48;5;226m"; /* Yellow Rainbow */
			colors['#']  = "\033[48;5;118m"; /* Green rainbow */
			colors['=']  = "\033[48;5;33m";  /* Light blue rainbow */
			colors[';']  = "\033[48;5;19m";  /* Dark blue rainbow */
			colors['*']  = "\033[48;5;240m"; /* Gray cat face */
			colors['%']  = "\033[48;5;175m"; /* Pink cheeks */
			break;
		case 2:
			colors[',']  = "\033[104m";      /* Blue background */
			colors['.']  = "\033[107m";      /* White stars */
			colors['\''] = "\033[40m";       /* Black border */
			colors['@']  = "\033[47m";       /* Tan poptart */
			colors['$']  = "\033[105m";      /* Pink poptart */
			colors['-']  = "\033[101m";      /* Red poptart */
			colors['>']  = "\033[101m";      /* Red rainbow */
			colors['&']  = "\033[43m";       /* Orange rainbow */
			colors['+']  = "\033[103m";      /* Yellow Rainbow */
			colors['#']  = "\033[102m";      /* Green rainbow */
			colors['=']  = "\033[104m";      /* Light blue rainbow */
			colors[';']  = "\033[44m";       /* Dark blue rainbow */
			colors['*']  = "\033[100m";      /* Gray cat face */
			colors['%']  = "\033[105m";      /* Pink cheeks */
			break;
		case 3:
			colors[',']  = "\033[25;44m";    /* Blue background */
			colors['.']  = "\033[5;47m";     /* White stars */
			colors['\''] = "\033[25;40m";    /* Black border */
			colors['@']  = "\033[5;47m";     /* Tan poptart */
			colors['$']  = "\033[5;45m";     /* Pink poptart */
			colors['-']  = "\033[5;41m";     /* Red poptart */
			colors['>']  = "\033[5;41m";     /* Red rainbow */
			colors['&']  = "\033[25;43m";    /* Orange rainbow */
			colors['+']  = "\033[5;43m";     /* Yellow Rainbow */
			colors['#']  = "\033[5;42m";     /* Green rainbow */
			colors['=']  = "\033[25;44m";    /* Light blue rainbow */
			colors[';']  = "\033[5;44m";     /* Dark blue rainbow */
			colors['*']  = "\033[5;40m";     /* Gray cat face */
			colors['%']  = "\033[5;45m";     /* Pink cheeks */
			break;
		case 4:
			colors[',']  = "\033[0;34;44m";  /* Blue background */
			colors['.']  = "\033[1;37;47m";  /* White stars */
			colors['\''] = "\033[0;30;40m";  /* Black border */
			colors['@']  = "\033[1;37;47m";  /* Tan poptart */
			colors['$']  = "\033[1;35;45m";  /* Pink poptart */
			colors['-']  = "\033[1;31;41m";  /* Red poptart */
			colors['>']  = "\033[1;31;41m";  /* Red rainbow */
			colors['&']  = "\033[0;33;43m";  /* Orange rainbow */
			colors['+']  = "\033[1;33;43m";  /* Yellow Rainbow */
			colors['#']  = "\033[1;32;42m";  /* Green rainbow */
			colors['=']  = "\033[1;34;44m";  /* Light blue rainbow */
			colors[';']  = "\033[0;34;44m";  /* Dark blue rainbow */
			colors['*']  = "\033[1;30;40m";  /* Gray cat face */
			colors['%']  = "\033[1;35;45m";  /* Pink cheeks */
			output = "██";
			break;
		case 5:
			colors[',']  = "\033[0;34;44m";  /* Blue background */
			colors['.']  = "\033[1;37;47m";  /* White stars */
			colors['\''] = "\033[0;30;40m";  /* Black border */
			colors['@']  = "\033[1;37;47m";  /* Tan poptart */
			colors['$']  = "\033[1;35;45m";  /* Pink poptart */
			colors['-']  = "\033[1;31;41m";  /* Red poptart */
			colors['>']  = "\033[1;31;41m";  /* Red rainbow */
			colors['&']  = "\033[0;33;43m";  /* Orange rainbow */
			colors['+']  = "\033[1;33;43m";  /* Yellow Rainbow */
			colors['#']  = "\033[1;32;42m";  /* Green rainbow */
			colors['=']  = "\033[1;34;44m";  /* Light blue rainbow */
			colors[';']  = "\033[0;34;44m";  /* Dark blue rainbow */
			colors['*']  = "\033[1;30;40m";  /* Gray cat face */
			colors['%']  = "\033[1;35;45m";  /* Pink cheeks */
			output = "\333\333";
			break;
		case 6:
			colors[',']  = "::";             /* Blue background */
			colors['.']  = "@@";             /* White stars */
			colors['\''] = "  ";             /* Black border */
			colors['@']  = "##";             /* Tan poptart */
			colors['$']  = "??";             /* Pink poptart */
			colors['-']  = "<>";             /* Red poptart */
			colors['>']  = "##";             /* Red rainbow */
			colors['&']  = "==";             /* Orange rainbow */
			colors['+']  = "--";             /* Yellow Rainbow */
			colors['#']  = "++";             /* Green rainbow */
			colors['=']  = "~~";             /* Light blue rainbow */
			colors[';']  = "$$";             /* Dark blue rainbow */
			colors['*']  = ";;";             /* Gray cat face */
			colors['%']  = "()";             /* Pink cheeks */
			always_escape = 1;
			break;
		case 7:
			colors[',']  = ".";             /* Blue background */
			colors['.']  = "@";             /* White stars */
			colors['\''] = " ";             /* Black border */
			colors['@']  = "#";             /* Tan poptart */
			colors['$']  = "?";             /* Pink poptart */
			colors['-']  = "O";             /* Red poptart */
			colors['>']  = "#";             /* Red rainbow */
			colors['&']  = "=";             /* Orange rainbow */
			colors['+']  = "-";             /* Yellow Rainbow */
			colors['#']  = "+";             /* Green rainbow */
			colors['=']  = "~";             /* Light blue rainbow */
			colors[';']  = "$";             /* Dark blue rainbow */
			colors['*']  = ";";             /* Gray cat face */
			colors['%']  = "o";             /* Pink cheeks */
			always_escape = 1;
			terminal_width = 40;
			break;
		case 8:
			colors[',']  = "\033[48;2;0;49;105m";    /* Blue background */
			colors['.']  = "\033[48;2;255;255;255m"; /* White stars */
			colors['\''] = "\033[48;2;0;0;0m";       /* Black border */
			colors['@']  = "\033[48;2;255;205;152m"; /* Tan poptart */
			colors['$']  = "\033[48;2;255;169;255m"; /* Pink poptart */
			colors['-']  = "\033[48;2;255;76;152m";  /* Red poptart */
			colors['>']  = "\033[48;2;255;25;0m";    /* Red rainbow */
			colors['&']  = "\033[48;2;255;154;0m";   /* Orange rainbow */
			colors['+']  = "\033[48;2;255;240;0m";   /* Yellow Rainbow */
			colors['#']  = "\033[48;2;40;220;0m";    /* Green rainbow */
			colors['=']  = "\033[48;2;0;144;255m";   /* Light blue rainbow */
			colors[';']  = "\033[48;2;104;68;255m";  /* Dark blue rainbow */
			colors['*']  = "\033[48;2;153;153;153m"; /* Gray cat face */
			colors['%']  = "\033[48;2;255;163;152m"; /* Pink cheeks */
			break;
		default:
			break;
	}

	if (min_col == max_col) {
		min_col = (FRAME_WIDTH - terminal_width/2) / 2;
		max_col = (FRAME_WIDTH + terminal_width/2) / 2;
		using_automatic_width = 1;
	}

	if (min_row == max_row) {
		min_row = (FRAME_HEIGHT - (terminal_height-1)) / 2;
		max_row = (FRAME_HEIGHT + (terminal_height-1)) / 2;
		using_automatic_height = 1;
	}

	/* Attempt to set terminal title */
	if (set_title) {
		printf("\033kNyanyanyanyanyanyanya...\033\134");
		printf("\033]1;Nyanyanyanyanyanyanya...\007");
		printf("\033]2;Nyanyanyanyanyanyanya...\007");
	}

	if (clear_screen) {
		/* Clear the screen */
		printf("\033[H\033[2J\033[?25l");
	} else {
		printf("\033[s");
	}

	if (show_intro) {
		/* Display the MOTD */
		unsigned int countdown_clock = 5;
		for (k = 0; k < countdown_clock; ++k) {
			newline(3);
			printf("                             \033[1mNyancat Telnet Server\033[0m");
			newline(2);
			printf("                   written and run by \033[1;32mK. Lange\033[1;34m @_klange\033[0m");
			newline(2);
			printf("        If things don't look right, try:");
			newline(1);
			printf("                TERM=fallback telnet ...");
			newline(2);
			printf("        Or on Windows:");
			newline(1);
			printf("                telnet -t vtnt ...");
			newline(2);
			printf("        Problems? Check the website:");
			newline(1);
			printf("                \033[1;34mhttp://nyancat.dakko.us\033[0m");
			newline(2);
			printf("        This is a telnet server, remember your escape keys!");
			newline(1);
			printf("                \033[1;31m^]quit\033[0m to exit");
			newline(2);
			printf("        Starting in %d...                \n", countdown_clock-k);

			fflush(stdout);
			usleep(400000);
			if (clear_screen) {
				printf("\033[H"); /* Reset cursor */
			} else {
				printf("\033[u");
			}
		}

		if (clear_screen) {
			/* Clear the screen again */
			printf("\033[H\033[2J\033[?25l");
		}
	}

	if (listen_mode) {
		run_telnet_server();
		return 0;
	}

	/* Add this section for standalone mode */
	time_t start, current;
	time(&start);

	int playing = 1;    /* Animation should continue */
	size_t i = 0;       /* Current frame # */
	unsigned int f = 0;  /* Total frames passed */
	char last = 0;      /* Last color index rendered */

	while (playing) {
		if (clear_screen) {
			printf("\033[H");
		} else {
			printf("\033[u");
		}
		
		for (int y = min_row; y < max_row; ++y) {
			for (int x = min_col; x < max_col; ++x) {
				char color;
				if (y > 23 && y < 43 && x < 0) {
					int mod_x = ((-x+2) % 16) / 8;
					if ((i / 2) % 2) {
						mod_x = 1 - mod_x;
					}
					const char *rainbow = ",,>>&&&+++###==;;;,,";
					color = rainbow[mod_x + y-23];
					if (color == 0) color = ',';
				} else if (x < 0 || y < 0 || y >= FRAME_HEIGHT || x >= FRAME_WIDTH) {
					color = ',';
				} else {
					color = frames[i][y][x];
				}
				if (always_escape) {
					printf("%s", colors[(int)color]);
				} else {
					if (color != last && colors[(int)color]) {
						last = color;
						printf("%s%s", colors[(int)color], output);
					} else {
						printf("%s", output);
					}
				}
			}
			newline(1);
		}

		if (show_counter) {
			time(&current);
			double diff = difftime(current, start);
			int nLen = digits((int)diff);
			int width = (terminal_width - 29 - nLen) / 2;
			while (width > 0) {
				printf(" ");
				width--;
			}
			printf("\033[1;37mYou have nyaned for %0.0f seconds!\033[J\033[0m", diff);
		}

		last = 0;
		++f;
		if (frame_count != 0 && f == frame_count) {
			break;
		}
		++i;
		if (!frames[i]) {
			i = 0;
		}
		usleep(1000 * delay_ms);
	}

	return 0;
}
