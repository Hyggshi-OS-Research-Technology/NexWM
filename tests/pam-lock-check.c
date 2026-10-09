/* A tiny test-only PAM module: request the password using PAM's normal conversation and accept one known test value. */
#define PAM_SM_AUTH
#include <security/pam_appl.h>
#include <security/pam_ext.h>
#include <security/pam_modules.h>
#include <stdlib.h>
#include <string.h>

PAM_EXTERN int pam_sm_authenticate(pam_handle_t *pamh, int flags, int argc, const char **argv)
{
    (void)flags;
    (void)argc;
    (void)argv;
    const char *username = NULL;
    int rc = pam_get_user(pamh, &username, NULL);
    if (rc != PAM_SUCCESS) return rc;
    char *entered_username = NULL;
    rc = pam_prompt(pamh, PAM_PROMPT_ECHO_ON, &entered_username, "Login: ");
    if (rc != PAM_SUCCESS) return rc;
    int username_ok = username && entered_username && strcmp(username, entered_username) == 0;
    free(entered_username);
    if (!username_ok) return PAM_AUTH_ERR;

    const char *password = NULL;
    rc = pam_get_authtok(pamh, PAM_AUTHTOK, &password, "Password: ");
    if (rc != PAM_SUCCESS) return rc;
    return password && strcmp(password, "hde-lock-test-pass-42") == 0 ? PAM_SUCCESS : PAM_AUTH_ERR;
}

PAM_EXTERN int pam_sm_setcred(pam_handle_t *pamh, int flags, int argc, const char **argv)
{
    (void)pamh;
    (void)flags;
    (void)argc;
    (void)argv;
    return PAM_SUCCESS;
}
