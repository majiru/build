#include <u.h>
#include <libc.h>
#include <ctype.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>

typedef struct Tile Tile;
typedef struct XFile XFile;
typedef struct Grp Grp;

enum { Qfile, Qtile };

struct XFile {
	char type;
	char name[12+1];
	ushort x, y;
	ulong size; /* for Qtile size is of image(6) buffer */
	ulong off;
	uchar *buf;
};

struct Grp {
	int fd;
	int nf;
	XFile *f;
	int nt;
	XFile *tiles;
	uint pal[256];
};

ulong
get16(uchar *b)
{
	ulong x;

	x = (b[0]<<0) | (b[1]<<8);
	return x;
}

ulong
get32(uchar *b)
{
	ulong x;

	x = (b[0]<<0) | (b[1]<<8) | (b[2]<<16) | (b[3]<<24);
	return x;
}

Grp ggrp;

static void
fsreadtile(Req *r)
{
	XFile *f;
	static uchar buf[64*1024];
	uint *dot;
	int i;

	f = r->fid->file->aux;
	if(f->buf == nil){
		f->buf = mallocz((11 * 5) + 5 + (f->x * f->y * 4), 1);
		sprint((char*)f->buf, "%11s %11d %11d %11d %11d", "a8r8g8b8", 0, 0, f->x, f->y);
		dot = (uint*)(f->buf + (11 * 5) + 5);
		pread(ggrp.fd, buf, f->x * f->y, f->off);
		for(i = 0; i < f->x * f->y; i++){
			dot[i] = ggrp.pal[buf[i]];
		}
		f->size = (11 * 5) + 5 + (f->x * f->y * 4);
	}
	readbuf(r, f->buf, f->size);
	respond(r, nil);
}

static void
fsread(Req *r)
{
	XFile *f;

	f = r->fid->file->aux;

	if(f->type == Qtile){
		fsreadtile(r);
		return;
	}

	r->ofcall.count = r->ifcall.count;
	if(r->ifcall.offset >= f->size){
		r->ofcall.count = 0;
		respond(r, nil);
		return;
	}
	if(r->ifcall.offset+r->ofcall.count > f->size)
		r->ofcall.count = f->size - r->ifcall.offset;
	r->ofcall.count = pread(ggrp.fd, r->ofcall.data, r->ofcall.count, r->ifcall.offset+f->off);
	respond(r, nil);
}

Srv fs = {
	.read=fsread,
};

void
parseart(int fd, ulong off)
{
	static short tilesx[1024];
	static short tilesy[1024];
	uchar buf[16];
	XFile *f;
	int nt, i, n;
	ulong start;

	start = off;
	n = pread(fd, buf, 16, start);
	off += n;
	nt = get32(buf+12) - get32(buf+8) + 1;

	for(i = 0; i < nt; i++){
		pread(fd, buf, 2, start + 16 + (i * 2));
		tilesx[i] = get16(buf);
	}
	for(i = 0; i < nt; i++){
		pread(fd, buf, 2, start + 16 + ((i+nt) * 2));
		tilesy[i] = get16(buf);
	}
	off += (nt * 2) + (nt * 2) + (nt * 4);

	ggrp.tiles = realloc(ggrp.tiles, sizeof(XFile) * (ggrp.nt + nt));
	memset(&ggrp.tiles[ggrp.nt], 0, nt * sizeof(XFile));
	f = ggrp.tiles + ggrp.nt;
	for(i = 0; i < nt; i++,f++){
		f->type = Qtile;
		f->x = tilesx[i];
		f->y = tilesy[i];
		f->off = off;
		off += tilesx[i] * tilesy[i];
	}
	ggrp.nt += nt;
}

void
parsepal(int fd, int off)
{
	uchar buf[256 * 3];
	int i;

	memset(ggrp.pal, 0, sizeof ggrp.pal);
	pread(fd, buf, sizeof buf, off);
	for(i = 0; i < sizeof buf; i += 3){
		//print("%ud %ud %ud\n", buf[i], buf[i+1], buf[i+2]);
		ggrp.pal[i/3] = ((buf[i+2]*4)<<0) | ((buf[i+1]*4)<<8) | ((buf[i]*4)<<16);
	}
}

void
parsegrp(int fd)
{
	char *kens = "KenSilverman";
	uchar buf[64];
	uchar *p;
	int i, n;
	ulong off;
	XFile *f;
	char *user;
	File *dir;

	off = n = read(fd, buf, strlen(kens));
	if(n < 0 || memcmp(buf, kens, n) != 0)
		sysfatal("invalid file");

	n = read(fd, buf, 4);
	if(n != 4)
		sysfatal("failed to read number of files");

	user = getenv("user");
	fs.tree = alloctree(user, "sys", 0644, nil);
	off += n;
	ggrp.fd = fd;
	ggrp.nf = get32((uchar*)buf);
	ggrp.f = f = mallocz(sizeof(XFile) * ggrp.nf, 1);
	for(i = 0; i < ggrp.nf; i++,f++){
		n = read(fd, buf, 12);
		buf[n+1] = 0;
		for(p = buf; p <= buf+n; p++)
			*p = tolower(*p);
		memcpy(f->name, buf, n);
		off += n;
		n = read(fd, buf, 4);
		off += n;
		f->size = get32((uchar*)buf);
	}

	ggrp.nt = 0;
	f = ggrp.f;
	for(i = 0; i < ggrp.nf; i++,f++){
		f->off = off;
		if(strstr(f->name, ".art") != nil)
			parseart(fd, off);
		else
			createfile(fs.tree->root, f->name, user, 0666, f);

		if(strcmp(f->name, "palette.dat") == 0)
			parsepal(fd, off);
		off = f->off + f->size;
	}

	dir = createfile(fs.tree->root, "art", user, 0644|DMDIR, nil);
	for(i = 0; i < ggrp.nt; i++)
		createfile(dir, smprint("%05d.tile", i), user, 0666, ggrp.tiles + i);
	free(user);
}

static void
usage(void)
{
	fprint(2, "usage: %s grpfile\n", argv0);
	exits("usage");
}

void
main(int argc, char **argv)
{
	char *mtpt, *srvname;
	int fd;

	srvname = nil;
	mtpt = "/mnt/grp";
	ARGBEGIN{
	case 's':
		srvname = EARGF(usage());
		break;
	case 'm':
		mtpt = EARGF(usage());
		break;
	default:
		usage();
		break;
	}ARGEND;

	if(argc < 1)
		usage();

	fd = open(argv[0], OREAD);
	if(fd < 0)
		sysfatal("%r");

	parsegrp(fd);
	postmountsrv(&fs, srvname, mtpt, MREPL);
	exits(nil);
}
