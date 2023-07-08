#include <fonts/FontAtlas.h>
#include <zephyr/shell/shell.h>

FontAtlas font;

static int cmd_font_print(const struct shell *shell,
                               size_t argc, char **argv, void *data)
{
    if (argc != 2) {
        shell_error(shell, "Invalid number of arguments %d", argc);
        return -ENOEXEC;
    }

    char* ch = argv[1];
    
    font.DebugChar(ch[0]);

    return 0;
}

// Subcommands for "power"
SHELL_STATIC_SUBCMD_SET_CREATE(sub_font,
                               SHELL_CMD_ARG(print, NULL, "Print a character from the font atlas", cmd_font_print, 2, 0),
                               SHELL_SUBCMD_SET_END);


/* Creating root (level 0) command "font" */
SHELL_CMD_REGISTER(font, &sub_font, "Font commands", NULL);