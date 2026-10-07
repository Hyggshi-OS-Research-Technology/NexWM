/* hde-svgpath.h — parse the "d" attribute of an SVG <path> (M L H V C S Q T A Z, absolute and relative) into
 * move / line / cubic curve / close calls. Enough to draw the logos of data/logos with cairo, without an SVG
 * library. Pure C (tests/svgpath-test.c checks it against reference drawings). */
#ifndef HDE_SVGPATH_H
#define HDE_SVGPATH_H

typedef struct {
    void (*move_to)(void *user, double x, double y);
    void (*line_to)(void *user, double x, double y);
    void (*curve_to)(void *user, double x1, double y1, double x2, double y2, double x3, double y3);
    void (*close_path)(void *user);
} HdeSvgPathSink;

/* Returns the number of drawing commands, or -1 if the path data is invalid (what came before is still emitted). */
int hde_svg_path_parse(const char *d, const HdeSvgPathSink *sink, void *user);

#endif
