/* app.h — command-line options shared by hde-cmd's CLI and GTK front end. */
#ifndef HDE_CMD_APP_H
#define HDE_CMD_APP_H

typedef struct {
    const char *working_directory;
    const char *title;
    char **command;        /* NULL means the user's interactive shell */
    int use_vte;           /* opt-in; the built-in VT engine is the normal backend */
} HdeCmdOptions;

int hde_cmd_ui_run(const HdeCmdOptions *options, const char *program_name);

#endif /* HDE_CMD_APP_H */
