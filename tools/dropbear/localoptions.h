/* dropbear build options for the SWEET/ginkgo iOS port (tools/dropbear). */
#define DROPBEAR_SVR_PASSWORD_AUTH 1
#define DROPBEAR_SVR_PUBKEY_AUTH 1
#define DROPBEAR_DSS 0
#define DROPBEAR_SMALL_CODE 0
#define DSS_PRIV_FILENAME "/var/jb/etc/dropbear/dropbear_dss_host_key"
#define RSA_PRIV_FILENAME "/var/jb/etc/dropbear/dropbear_rsa_host_key"
#define ECDSA_PRIV_FILENAME "/var/jb/etc/dropbear/dropbear_ecdsa_host_key"
#define ED25519_PRIV_FILENAME "/var/jb/etc/dropbear/dropbear_ed25519_host_key"
#define DEFAULT_PATH "/var/jb/usr/bin:/var/jb/bin:/var/jb/usr/sbin:/usr/bin:/bin:/usr/sbin:/sbin"
#define DEFAULT_ROOT_PATH DEFAULT_PATH
#define SFTPSERVER_PATH "/var/jb/usr/libexec/sftp-server"
