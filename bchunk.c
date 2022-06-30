
 /*
  *	binchunker for Unix
  *	Copyright (C) 1998-2004  Heikki Hannikainen <hessu@hes.iki.fi>
  *
  *  This program is free software; you can redistribute it and/or modify
  *  it under the terms of the GNU General Public License as published by
  *  the Free Software Foundation; either version 2 of the License, or
  *  (at your option) any later version.
  *
  *  This program is distributed in the hope that it will be useful,
  *  but WITHOUT ANY WARRANTY; without even the implied warranty of
  *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  *  GNU General Public License for more details.
  *
  *  You should have received a copy of the GNU General Public License
  *  along with this program; if not, write to the Free Software
  *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
  */

#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#define VERSION "1.2.3"
#define USAGE "Usage: bchunk [-t] [-v] [-r] [-p (PSX)] [-w (wav)] [-s (swabaudio)]\n" \
	"         <image.bin | track # | '*'> <image.cue> [ <basename> ]\n" \
	"Example: bchunk foo.bin foo.cue foo\n" \
	"         bchunk foo.bin foo.cue\n" \
	"         bchunk 2 foo.cue foo\n" \
	"  -t  Insert track # in between the basename and the format extension\n" \
	"  -v  Verbose mode\n" \
	"  -r  Raw mode for MODE2/2352: write all 2352 bytes from offset 0 (VCD/MPEG)\n" \
	"  -p  PSX mode for MODE2/2352: write 2336 bytes from offset 24\n" \
	"      (default MODE2/2352 mode writes 2048 bytes from offset 24)\n"\
	"  -w  Output audio files in WAV format\n" \
	"  -s  swabaudio: swap byte order in audio tracks\n"
	
#define VERSTR	"binchunker for Unix, version " VERSION " by Heikki Hannikainen <hessu@hes.iki.fi>\n" \
		"\tCreated with the kind help of Bob Marietta <marietrg@SLU.EDU>,\n" \
		"\tpartly based on his Pascal (Delphi) implementation.\n" \
		"\tSupport for MODE2/2352 ISO tracks thanks to input from\n" \
		"\tGodmar Back <gback@cs.utah.edu>, Colas Nahaboo <Colas@Nahaboo.com>\n" \
		"\tMatthew Green <mrg@eterna.com.au> & twojstaryzdomu <@github.com>.\n" \
		"\tReleased under the GNU GPL, version 2 or later (at your option).\n\n"

#define CUELLEN 1024
#define SECTLEN 2352

#define WAV_RIFF_HLEN 12
#define WAV_FORMAT_HLEN 24
#define WAV_DATA_HLEN 8
#define WAV_HEADER_LEN WAV_RIFF_HLEN + WAV_FORMAT_HLEN + WAV_DATA_HLEN

/*
 *	Ugly way to convert integers to little-endian format.
 *	First let netinet's hton() functions decide if swapping should
 *	be done, then convert back.
 */

#include <inttypes.h>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <netinet/in.h>
#endif

#define bswap_16(x) \
     ((((x) >> 8) & 0xff) | (((x) & 0xff) << 8))
#define bswap_32(x) \
     ((((x) & 0xff000000) >> 24) | (((x) & 0x00ff0000) >>  8) |  \
      (((x) & 0x0000ff00) <<  8) | (((x) & 0x000000ff) << 24))

#define htoles(x) bswap_16(htons(x))
#define htolel(x) bswap_32(htonl(x))



struct track_t {
	int num;
	int mode;
	int audio;
	char *file;
	char *output;
	char *modes;
	char *extension;
	int bstart;
	int bsize;
	long startsect;
	long stopsect;
	long start;
	long stop;
	struct track_t *next;
};

char *basefile = NULL;
char *binfile = NULL;
char *cuefile = NULL;
char *bname = NULL;
char *file = NULL;
int verbose = 0;
int psxtruncate = 0;
int raw = 0;
int swabaudio = 0;
int towav = 0;
int trackadd = 0;
FILE *binf, *cuef;
struct track_t *tracks = NULL;
struct track_t *track = NULL;

void free_all(void)
{
	if (binf)
		fclose(binf);
	if (cuef)
		fclose(cuef);
	struct track_t *next;
	for (track = tracks; (track); track = next) {
		next = track->next;
		free(track->file);
		free(track->output);
		free(track->modes);
		free(track);
	}
	free(basefile);
	free(binfile);
	free(cuefile);
	free(file);
	free(bname);
}

void die_format(int status, char *format, ...)
{
	va_list args;
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
	free_all();
	exit(status);
}
#define die(status, error) die_format(status, "%s", error)

/*
 *	Copy a string until the final '.'
 */

char* prune_ext(char *src)
{
	char *dst;
	char *e = strrchr(src, '.');
	int l = e ? e - src : strlen(src);
	if (!(dst = malloc(l + 1)))
		die(4, "prune_ext(): malloc() failed, out of memory\n");
	dst[l] = '\0';
	return strncpy(dst, src, l);
}

/*
 *	Parse arguments
 */

void parse_args(int argc, char *argv[])
{
	int s;
	
	while ((s = getopt(argc, argv, "swvp?hrt")) != -1) {
		switch (s) {
			case 'r':
				raw = 1;
				break;
			case 'v':
				verbose = 1;
				break;
			case 'w':
				towav = 1;
				break;
			case 'p':
				psxtruncate = 1;
				break;
			case 's':
				swabaudio = 1;
				break;
			case 't':
				trackadd = 1;
				break;
			case '?':
			case 'h':
				die_format(0, "%s", USAGE);
		}
	}

	if (argc - optind < 2)
		die_format(1, "%s", USAGE);
	
	for (int i = optind; i < argc; i++) {
		switch (i - optind) {
			case 0:
				binfile = strdup(argv[i]);
				break;
			case 1:
				cuefile = strdup(argv[i]);
				break;
			case 2:
				basefile = strdup(argv[i]);
				break;
			default:
				die_format(1, "Gratuitous argument %s\n%s", argv[i], USAGE);
		}
	}
}

/*
 *	Convert a mins:secs:frames format to plain frames
 */

long time2frames(char *s)
{
	int mins = 0, secs = 0, frames = 0;
	char *p, *t;
	
	if (!(p = strchr(s, ':')))
		return -1;
	*p = '\0';
	mins = atoi(s);
	
	p++;
	if (!(t = strchr(p, ':')))
		return -1;
	*t = '\0';
	secs = atoi(p);
	
	t++;
	frames = atoi(t);
	
	return 75 * (mins * 60 + secs) + frames;
}

/*
 *	Parse the mode string
 */

void gettrackmode(struct track_t *track, char *modes)
{
	static char ext_iso[] = "iso";
	static char ext_cdr[] = "cdr";
	static char ext_wav[] = "wav";
	static char ext_ugh[] = "ugh";
	
	track->audio = 0;
	
	if (!strcasecmp(modes, "MODE1/2352")) {
		track->bstart = 16;
		track->bsize = 2048;
		track->extension = ext_iso;
		
	} else if (!strcasecmp(modes, "MODE2/2352")) {
		track->extension = ext_iso;
		if (raw) {
			/* Raw MODE2/2352 */
			track->bstart = 0;
			track->bsize = 2352;
		} else if (psxtruncate) {
			/* PSX: truncate from 2352 to 2336 byte tracks */
			track->bstart = 0;
			track->bsize = 2336;
		} else {
			/* Normal MODE2/2352 */
			track->bstart = 24;
			track->bsize = 2048;
		}
		
	} else if (!strcasecmp(modes, "MODE2/2336")) {
		/* WAS 2352 in V1.361B still work?
		 * what if MODE2/2336 single track bin, still 2352 sectors?
		 */
		track->bstart = 16;
		track->bsize = 2336;
		track->extension = ext_iso;
		
	} else if (!strcasecmp(modes, "AUDIO")) {
		track->bstart = 0;
		track->bsize = 2352;
		track->audio = 1;
		if (towav)
			track->extension = ext_wav;
		else
			track->extension = ext_cdr;
	} else {
		printf("(?) ");
		track->bstart = 0;
		track->bsize = 2352;
		track->extension = ext_ugh;
	}
}

/*
 *	return a progress bar
 */

char *progressbar(float f, int l)
{
	static char s[80];
	int i, n;
	
	n = l * f;
	for (i = 0; i < n; i++) {
		s[i] = '*';
	}
	for (; i < l; i++) {
		s[i] = ' ';
	}
	s[i] = '\0';
	
	return s;
}

/*
 *	Write a track
 */

int writetrack(FILE *bf, struct track_t *track)
{
	FILE *f;
	char buf[SECTLEN+10];
	long sz, sect, realsz, reallen;
	char c, *p, *p2, *ep;
	int32_t l;
	int16_t i;
	float fl;
	
	printf("%2d: %s ", track->num, track->output);
	
	if (!(f = fopen(track->output, "wb")))
		die_format(4, " Could not fopen track file: %s\n", strerror(errno));
	
	if (fseek(bf, track->start, SEEK_SET))
		die_format(4, " Could not fseek to track location: %s\n", strerror(errno));
	
	reallen = (track->stopsect - track->startsect + 1) * track->bsize;
	if (verbose) {
		printf(	"\n mmc sectors %ld->%ld (%ld)"
			"\n mmc bytes %ld->%ld (%ld)"
			"\n sector data at %d, %d bytes per sector"
			"\n real data %ld bytes\n",
			track->startsect, track->stopsect, track->stopsect - track->startsect + 1,
			track->start, track->stop, track->stop - track->start + 1,
			track->bstart, track->bsize,
			reallen);
	}

	printf("                                          ");
	
	if ((track->audio) && (towav)) {
		// RIFF header
		fputs("RIFF", f);
		l = htolel(reallen + WAV_DATA_HLEN + WAV_FORMAT_HLEN + 4);
		fwrite(&l, 4, 1, f);  // length of file, starting from WAVE
		fputs("WAVE", f);
		// FORMAT header
		fputs("fmt ", f);
		l = htolel(0x10);     // length of FORMAT header
		fwrite(&l, 4, 1, f);
		i = htoles(0x01);     // constant
		fwrite(&i, 2, 1, f);
		i = htoles(0x02);	// channels
		fwrite(&i, 2, 1, f);
		l = htolel(44100);	// sample rate
		fwrite(&l, 4, 1, f);
		l = htolel(44100 * 4);	// bytes per second
		fwrite(&l, 4, 1, f);
		i = htoles(4);		// bytes per sample
		fwrite(&i, 2, 1, f);
		i = htoles(2*8);	// bits per channel
		fwrite(&i, 2, 1, f);
		// DATA header
		fputs("data", f);
		l = htolel(reallen);
		fwrite(&l, 4, 1, f);
	}
	
	realsz = 0;
	sz = track->start;
	sect = track->startsect;
	fl = 0;
	while ((sect <= track->stopsect) && (fread(buf, SECTLEN, 1, bf) > 0)) {
		if (track->audio) {
			if (swabaudio) {
				/* swap low and high bytes */
				p = &buf[track->bstart];
				ep = p + track->bsize;
				while (p < ep) {
					p2 = p + 1;
					c = *p;
					*p = *p2;
					*p2 = c;
					p += 2;
				}
			}
		}
		if (fwrite(&buf[track->bstart], track->bsize, 1, f) < 1)
			die_format(4, " Could not write to track: %s\n", strerror(errno));
		sect++;
		sz += SECTLEN;
		realsz += track->bsize;
		if (((sz / SECTLEN) % 500) == 0) {
			fl = (float)realsz / (float)reallen;
			printf("\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b%4ld/%-4ld MB  [%s] %3.0f %%", realsz/1024/1024, reallen/1024/1024, progressbar(fl, 20), fl * 100);
			fflush(stdout);
		}
	}
	
	fl = (float)realsz / (float)reallen;
	printf("\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b%4ld/%-4ld MB  [%s] %3.0f %%", realsz/1024/1024, reallen/1024/1024, progressbar(1, 20), fl * 100);
	fflush(stdout);
	
	if (ferror(bf))
		die_format(4, " Could not read from %s: %s\n", binfile, strerror(errno));
	if (fclose(f))
		die_format(4, " Could not fclose track file: %s\n", strerror(errno));
	
	printf("\n");
	return 0;
}

/*
 *     Sets output filename
 */

int set_output(struct track_t *track, char *binfile, char *basefile, int trackadd) {
	char *t;
	if (strchr(binfile, '*')) {
		if (!basefile)
			bname = prune_ext(track->file);
		else
			if (asprintf(&bname, "%s", basefile) == -1)
				die(4, "set_output(): asprintf() failed, out of memory\n");
	} else if (strcmp(track->file, binfile) != 0) {
		long num = strtol(binfile, &t, 10);
		if (strlen(t) || num != track->num) {
			printf("%2d: skipped: %s not matching FILE or TRACK from cuesheet\n", track->num, binfile);
			return 0;
		}
	}
	if (!bname) {
		if (!basefile)
			bname = prune_ext(binfile);
		else
			if (asprintf(&bname, "%s", basefile) == -1)
				die(4, "set_output(): asprintf() failed, out of memory\n");
	}
	if (trackadd) {
		if (bname)
			t = bname;
		if (asprintf(&bname, "%s%2.2d", bname ? bname : "", track->num) == -1)
			die(4, "set_output(): asprintf() failed, out of memory\n");
		free(t);
	}
	if (asprintf(&track->output, "%s.%s", bname, track->extension) == -1)
		die(4, "set_output(): asprintf() failed, out of memory\n");
	free(bname);
	bname = NULL;
	return 1;
}

/*
 *	Main
 */

int main(int argc, char **argv)
{
	char s[CUELLEN+1];
	char *b, *e, *p, *t;
	struct track_t *prevtrack = NULL;
	struct track_t **prevp = &tracks;
	
	printf("%s", VERSTR);
	
	parse_args(argc, argv);
	
	if (!((cuef = fopen(cuefile, "r"))))
		die_format(2, "Could not open CUE %s: %s\n", cuefile, strerror(errno));
	
	if (verbose)
		printf("Include track # in output filename: %s\n\n", trackadd ? "yes" : "no");
	
	printf("Reading the CUE file:\n");
	
	while (fgets(s, CUELLEN, cuef)) {
		while ((p = strchr(s, '\r')) || (p = strchr(s, '\n')))
			*p = '\0';
			
		if ((p = strstr(s, "FILE"))) {
			if (!(b = strchr(p, ' ')))
				die(3, "... ouch, no space after FILE.\n");
			if (!(e = strrchr(p, ' ')))
				die(3, "... ouch, no space after filename.\n");
			if (e == b)
				die(3, "... ouch, no filename after FILE.\n");
			++b;
			if (b == strchr(b, '"'))
				b++;
			--e;
			if (e == strrchr(e, '"'))
				*e = '\0';
			if (e == b)
				die(3, "... ouch, empty filename entry.\n");
			if (file)
				free(file);
			if (asprintf(&file, "%s", b) == -1)
				die(4, "main(): asprintf() failed, out of memory\n");
			printf("\nFile: %s", file);
		}
		if ((p = strstr(s, "TRACK"))) {
			printf("\nTrack ");
			if (!(p = strchr(p, ' ')))
				die(3, "... ouch, no space after TRACK.\n");
			p++;
			if (!(t = strchr(p, ' ')))
				die(3, "... ouch, no space after track number.\n");
			*t = '\0';
			
			prevtrack = track;
			if (!(track = malloc(sizeof(struct track_t))))
				die(4, "main(): malloc() failed, out of memory\n");
			*prevp = track;
			prevp = &track->next;
			track->next = NULL;
			track->num = atoi(p);
			
			p = t + 1;
			printf("%2d: %-12.12s ", track->num, p);
			if (asprintf(&track->file, "%s", file) == -1)
				die(4, "main(): asprintf() failed, out of memory\n");
			track->modes = strdup(p);
			track->extension = NULL;
			track->output = NULL;
			track->mode = 0;
			track->audio = 0;
			track->bsize = track->bstart = -1;
			track->bsize = -1;
			track->startsect = track->stopsect = -1;
			
			gettrackmode(track, p);
			
		} else if ((p = strstr(s, "INDEX"))) {
			if (!(p = strchr(p, ' ')))
				die(3, "... ouch, no space after INDEX.\n");
			p++;
			if (!(t = strchr(p, ' ')))
				die(3, "... ouch, no space after index number.\n");
			*t = '\0';
			t++;
			printf(" %s %s", p, t);
			track->startsect = time2frames(t);
			track->start = track->startsect * SECTLEN;
			if (verbose)
				printf(" (startsect %ld ofs %ld)", track->startsect, track->start);
			if ((prevtrack) && (prevtrack->stopsect < 0) && !(strcmp(prevtrack->file,track->file))) { // only when files match
				prevtrack->stopsect = track->startsect - 1;
				prevtrack->stop = track->start - 1;
			}
		}
	}
	
	printf("\n\nWriting tracks:\n\n");
	
	for (track = tracks; (track); track = track->next) {
		if (!set_output(track, binfile, basefile, trackadd))
			continue;
		if (!(binf = fopen(track->file, "rb"))) {
			fprintf(stderr, "Could not open BIN %s: %s\n", track->file, strerror(errno));
			continue;
		}
		if (track->stopsect < 0) { // if not set yet
			if (fseek(binf, 0, SEEK_END) == -1)
				die_format(4, "main(): fseek failure in %s\n", track->file);
			track->stop = ftell(binf) - 1;
			track->stopsect = track->stop / SECTLEN;
		}
		writetrack(binf, track);
		fclose(binf);
		binf = NULL;
	}
	
	free_all();
	return 0;
}

