#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>

typedef struct Tile Tile;
typedef struct XFile XFile;
typedef struct Grp Grp;

enum { Qfile, Qtile, Qgrp };

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

#define GET16(p) ((u16int)(p)[0] | (u16int)(p)[1]<<8)
#define GET32(p) ((u32int)(p)[0] | (u32int)(p)[1]<<8 | (u32int)(p)[2]<<16 | (u32int)(p)[3]<<24)
#define PUT16(p, u) ((p)[1] = (u)>>8, (p)[0] = (u))
#define PUT32(p, u) ((p)[3] = (u)>>24, (p)[2] = (u)>>16, (p)[1] = (u)>>8, (p)[0] = (u))

static Grp ggrp;
static char kens[] = "KenSilverman";

static void
fsreadtile(Req *r, XFile *f)
{
	static uchar buf[64*1024];
	uint *dot;
	int i;

	f = r->fid->file->aux;
	if(f->buf != nil){
	Service:
		readbuf(r, f->buf, f->size);
		respond(r, nil);
		return;
	}

	f->buf = mallocz((11 * 5) + 5 + (f->x * f->y * 4), 1);
	sprint((char*)f->buf, "%11s %11d %11d %11d %11d", "a8r8g8b8", 0, 0, f->x, f->y);
	dot = (uint*)(f->buf + (11 * 5) + 5);
	pread(ggrp.fd, buf, f->x * f->y, f->off);
	for(i = 0; i < f->x * f->y; i++)
		dot[i] = ggrp.pal[buf[i]];
	f->size = (11 * 5) + 5 + (f->x * f->y * 4);
	goto Service;
}

static void
fsreadfile(Req *r, XFile *f)
{
	if(f->buf){
		readbuf(r, f->buf, f->size);
		respond(r, nil);
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

static void
fsreadgrp(Req *r, XFile*)
{
	int i, j;
	XFile *f;
	vlong off;
	u32int count;
	uchar buf[8192];
	uchar *p;

	off = r->ifcall.offset;
	count = r->ifcall.count;
	if(off < 12 + 4){
		memcpy(buf, kens, 12);
		PUT32(buf+12, ggrp.nf);
		readbuf(r, buf, 16);
		respond(r, nil);
		return;
	}
	off -= 12 + 4;
	if(off < 16 * ggrp.nf){
		assert(sizeof buf > 16*ggrp.nf);
		p = buf;
		for(i = 0; i < ggrp.nf; i++){
			memcpy(p, ggrp.f[i].name, 12);
			for(j = 0; j < 12; j++)
				*p++ = toupper(*p);
			PUT32(p, ggrp.f[i].size);
			p+=4;
		}
		r->ifcall.offset = off;
		readbuf(r, buf, 16*ggrp.nf);
		respond(r, nil);
		return;
	}
	off -= 16*ggrp.nf;
	for(i = 0; i < ggrp.nf; i++){
		f = ggrp.f+i;
		if(off >= f->size){
			off -= f->size;
			continue;
		}
		if(count+off >= f->size)
			count = f->size-off;
		if(f->buf != nil){
			r->ifcall.offset = off;
			r->ifcall.count = count;
			readbuf(r, f->buf, f->size);
		} else
			r->ofcall.count = pread(ggrp.fd, r->ofcall.data, count, off+f->off);
		respond(r, nil);
		return;
	}
	r->ofcall.count = 0;
	respond(r, nil);
}

void (*readftab[])(Req*,XFile*) = {
	[Qfile] fsreadfile,
	[Qtile] fsreadtile,
	[Qgrp]  fsreadgrp,
};

static void
fsread(Req *r)
{
	XFile *f;

	f = r->fid->file->aux;
	if(f->type >= nelem(readftab))
		sysfatal("invalid type %d", f->type);
	readftab[f->type](r, f);
}

static void
fswritefile(Req *r, XFile *f)
{
	vlong i, n;

	if(f->buf == nil){
		f->buf = malloc(f->size);
		for(n = 0; n != f->size;){
			i = pread(ggrp.fd, f->buf + n, f->size - n, f->off + n);
			if(i < 0){
				responderror(r);
				return;
			}
			n += i;
		}
	}
	if(r->ifcall.count + r->ifcall.offset > f->size){
		f->size = r->ifcall.count + r->ifcall.offset;
		f->buf = realloc(f->buf, f->size);
	}
	memmove(f->buf + r->ifcall.offset, r->ifcall.data, r->ifcall.count);
	r->ofcall.count = r->ifcall.count;
	respond(r, nil);
}

void (*writeftab[])(Req*,XFile*) = {
	[Qfile] fswritefile,
	[Qtile] nil,
	[Qgrp]  nil,
};

static void
fswrite(Req *r)
{
	XFile *f;

	f = r->fid->file->aux;
	if(f->type >= nelem(writeftab))
		sysfatal("invalid type %d", f->type);
	if(writeftab[f->type] == nil)
		respond(r, "not supported");
	else
		writeftab[f->type](r, f);
}

Srv fs = {
	.read=fsread,
	.write=fswrite,
};

void
parseart(Biobuf *b, ulong off)
{
	static short tilesx[1024];
	static short tilesy[1024];
	uchar buf[16];
	XFile *f;
	int nt, i, n;

	Bseek(b, off, 0);
	n = Bread(b, buf, 16);
	off += n;
	nt = GET32(buf+12) - GET32(buf+8) + 1;

	for(i = 0; i < nt; i++){
		Bread(b, buf, 2);
		tilesx[i] = GET16(buf);
	}
	for(i = 0; i < nt; i++){
		Bread(b, buf, 2);
		tilesy[i] = GET16(buf);
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
	for(i = 0; i < sizeof buf; i += 3)
		ggrp.pal[i/3] = ((buf[i+2]*4)<<0) | ((buf[i+1]*4)<<8) | ((buf[i]*4)<<16);
}

void
parsegrp(int fd)
{
	uchar buf[64];
	uchar *p;
	int i, n;
	ulong off;
	XFile *f;
	char *user;
	File *dir;
	Biobuf *b;

	b = Bfdopen(fd, OREAD);
	off = n = Bread(b, buf, strlen(kens));
	if(n < 0 || memcmp(buf, kens, n) != 0)
		sysfatal("invalid file");

	n = Bread(b, buf, 4);
	if(n != 4)
		sysfatal("failed to read number of files");

	user = getenv("user");
	fs.tree = alloctree(user, "sys", 0644, nil);
	f = mallocz(sizeof *f, 1);
	f->type = Qgrp;
	createfile(fs.tree->root, "GRP", user, 0444, f);
	off += n;
	ggrp.fd = fd;
	ggrp.nf = GET32((uchar*)buf);
	ggrp.f = f = mallocz(sizeof(XFile) * ggrp.nf, 1);
	for(i = 0; i < ggrp.nf; i++,f++){
		n = Bread(b, buf, 12);
		buf[n+1] = 0;
		for(p = buf; p <= buf+n; p++)
			*p = tolower(*p);
		memcpy(f->name, buf, n);
		off += n;
		n = Bread(b, buf, 4);
		off += n;
		f->size = GET32((uchar*)buf);
	}

	ggrp.nt = 0;
	f = ggrp.f;
	for(i = 0; i < ggrp.nf; i++,f++){
		f->off = off;
		if(strstr(f->name, ".art") != nil)
			parseart(b, off);
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
