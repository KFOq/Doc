#include "usb_linux.h"
#include "md5.h"

/*
[PATCH 3.10 27/54] usb: xhci: Add support for URB_ZERO_PACKET to bulk/sg transfers
https://www.spinics.net/lists/kernel/msg2100618.html

commit 4758dcd19a7d9ba9610b38fecb93f65f56f86346
Author: Reyad Attiyat <reyad.attiyat@gmail.com>
Date:   Thu Aug 6 19:23:58 2015 +0300

    usb: xhci: Add support for URB_ZERO_PACKET to bulk/sg transfers

    This commit checks for the URB_ZERO_PACKET flag and creates an extra
    zero-length td if the urb transfer length is a multiple of the endpoint's
    max packet length.
*/
unsigned qusb_zlp_mode = 1; //MT7621 donot support USB ZERO PACKET
unsigned q_erase_all_before_download = 0;
int sahara_main(const char *firehose_dir, void *usb_handle, int edl_mode_05c69008);
int firehose_main (const char *firehose_dir, void *usb_handle, unsigned qusb_zlp_mode);
int stream_download(const char *firehose_dir, void *usb_handle, unsigned qusb_zlp_mode);
int usb2tcp_main(const void *usb_handle, int tcp_port, unsigned qusb_zlp_mode);

//process vals
static long long all_bytes_to_transfer = 0;    //need transfered
static long long transfer_bytes = 0;        //transfered bytes;

const char *g_part_upgrade = NULL;

int switch_to_edl_mode(void *usb_handle) {
    //DIAG commands used to switch the Qualcomm devices to EDL (Emergency download mode)
    unsigned char edl_cmd[] = {0x4b, 0x65, 0x01, 0x00, 0x54, 0x0f, 0x7e};
    //unsigned char edl_cmd[] = {0x3a, 0xa1, 0x6e, 0x7e}; //DL (download mode)
    unsigned char *pbuf = malloc(512);
    int rx_len;

     do {
        rx_len = qusb_noblock_read(usb_handle, pbuf , 512, 0, 1000);
    } while (rx_len > 0);

    dbg_time("switch to 'Emergency download mode'\n");
    rx_len = qusb_noblock_write(usb_handle, edl_cmd, sizeof(edl_cmd), sizeof(edl_cmd), 3000, 0);
    if (rx_len < 0)
        return 0;

    do {
        rx_len = qusb_noblock_read(usb_handle, pbuf , 512, 0, 3000);
        if (rx_len == sizeof(edl_cmd) && memcmp(pbuf, edl_cmd, sizeof(edl_cmd)) == 0) {
            dbg_time("successful, wait module reboot\n");
            free(pbuf);
            return 1;
        }
    } while (rx_len > 0);

    free(pbuf);
    return 0;
}

static void usage(int status, const char *program_name)
{
    if(status != EXIT_SUCCESS)
    {
        printf("Try '%s --help' for more information.\n", program_name);
    }
    else
    {
        dbg_time("Upgrade Quectel's modules with Qualcomm's firehose protocol.\n");
        dbg_time("Usage: %s [options...]\n", program_name);
        dbg_time("    -f [package_dir]               Upgrade package directory path\n");
        dbg_time("    -p [/dev/ttyUSBx]              Diagnose port, will auto-detect if not specified\n");
        dbg_time("    -s [/sys/bus/usb/devices/xx]   When multiple modules exist on the board, use -s specify which module you want to upgrade\n");
        dbg_time("    -e                             Erase All Before Download (will Erase calibration data, careful to USE)\n");
    }
    exit(status);
}

/*
1. enum dir, fix up dirhose_dir
2. md5 examine
3. furture
*/
static int system_ready(char** dirhose_dir)
{
    char temp[255+2];

    if(strstr(*dirhose_dir, "firehose") != NULL)
    {
    }else
    {
        //set_transfer_allbytes(calc_filesizes(*dirhose_dir));
        sprintf(temp, "%s/update/firehose", *dirhose_dir);
        if(access(temp, R_OK))
            error_return();
        free(*dirhose_dir);
        *dirhose_dir = strdup(temp);
        return 0;
    }
    error_return();
}

#ifdef FIREHOSE_ENABLE
int firehose_main_entry(int argc, char* argv[])
#else
int main(int argc, char* argv[])
#endif
{
    int opt;
    int retval;
    void *usb_handle = NULL;
    int idVendor = 0, idProduct = 0, interfaceNum = 0;
    int edl_retry = 30; //SDX55 require long time by now 20190412
    double start;
    char *firehose_dir = malloc(MAX_PATH);
    char *module_port_name = malloc(MAX_PATH);
    char *module_sys_path = malloc(MAX_PATH);
    int xhci_usb3_to_usb2_cause_syspatch_chage = 1;
    int usb2tcp_port = 0;

    firehose_dir[0] = module_port_name[0] = module_sys_path[0] = '\0';

    dbg_time("QFirehose Version: Quectel_LTE&5G_QFirehose_Linux&Android_V1.2\n");
    dbg_time("Builded: %s %s\n", __DATE__,__TIME__);

    optind = 1;
    while ( -1 != (opt = getopt(argc, argv, "f:p:z:s:eh"))) {
        switch (opt) {
            case 'f':
                strncpy(firehose_dir, optarg, MAX_PATH);
            break;
            case 'p':
                strncpy(module_port_name, optarg, MAX_PATH);
                if (!strcmp(module_port_name, "9008")) {
                    usb2tcp_port = atoi(module_port_name);
                    module_port_name[0] = '\0';
                }
            break;
            case 's':
                xhci_usb3_to_usb2_cause_syspatch_chage = 0;
                strncpy(module_sys_path, optarg, MAX_PATH);
                if (module_sys_path[strlen(optarg)-1] == '/')
                    module_sys_path[strlen(optarg)-1] = '\0';
                break;
            case 'z':
                qusb_zlp_mode = !!atoi(optarg);
                break;
            case 'e':    
                q_erase_all_before_download = 1;
                break;
            case 'h':
                usage(EXIT_SUCCESS, argv[0]);
                break;
            default:
            break;
        }
    }

    if (usb2tcp_port)
        goto _usb2tcp_start;

    if (firehose_dir[0] == '\0') {
        usage(EXIT_SUCCESS, argv[0]);
        error_return();
    }
        
    if (access(firehose_dir, R_OK)) {
        dbg_time("fail to access %s, errno: %d (%s)\n", firehose_dir, errno, strerror(errno));
        error_return();
    }

    opt = strlen(firehose_dir);
    if (firehose_dir[opt-1] == '/') {
        firehose_dir[opt-1] = '\0';
    }
    
    if (!g_part_upgrade) {
        struct stat st;
        const char *update_dir = "/update/";
        char *update_pos = strstr(firehose_dir, update_dir);

        if (update_pos && lstat(firehose_dir, &st) == 0 && S_ISDIR(st.st_mode) == 0) {
            *update_pos = '\0';            

            g_part_upgrade = update_pos + strlen(update_dir);
        }
    }
    
    // check the md5 value of the upgrade file
    if (!g_part_upgrade && md5_check(firehose_dir))
        error_return();

    //hunter.lv add check dir 2018-07-28
    if(system_ready(&firehose_dir))
        error_return();
    //hunter.lv add check dir 2018-07-28

    if (module_port_name[0] && !strncmp(module_port_name, "/dev/mhi", strlen("/dev/mhi"))) {
        if (qpcie_open(firehose_dir)) {
            error_return();
        }      
		
        usb_handle = &edl_pcie_mhifd;
        start = get_now();
        goto __firehose_main;
    }
    else if (module_port_name[0] && strstr(module_port_name, ":9008")) {
        strcpy(module_sys_path, module_port_name);
        goto __edl_retry;
    }

_usb2tcp_start:
    if (module_sys_path[0] && access(module_sys_path, R_OK)) {
        dbg_time("fail to access %s, errno: %d (%s)\n", module_sys_path, errno, strerror(errno));
        error_return();
    }

    if (module_port_name[0] && access(module_port_name, R_OK | W_OK)) {
        dbg_time("fail to access %s, errno: %d (%s)\n", module_port_name, errno, strerror(errno));
        error_return();
    }

    if (module_sys_path[0] == '\0' && module_port_name[0] != '\0') {
        //get sys path by port name
        quectel_get_syspath_name_by_ttyport(module_port_name, module_sys_path, MAX_PATH);
    }

    if (module_sys_path[0] == '\0') {
         int module_count = auto_find_quectel_modules(module_sys_path, MAX_PATH);

        if (module_count <= 0) {
            dbg_time("Quectel module not found\n");
            error_return();
        }
        else if (module_count == 1) {

        } else {
            dbg_time("There are multiple quectel modules in system, Please use <-s /sys/bus/usb/devices/xx> specify which module you want to upgrade!\n");
            dbg_time("The module's </sys/bus/usb/devices/xx> ptah was printed in the previous log!\n");
            error_return();
        }
    }

__edl_retry:
    while (edl_retry-- > 0) {
        usb_handle = qusb_noblock_open(module_sys_path, &idVendor, &idProduct, &interfaceNum);
        if (usb_handle == NULL && module_sys_path[0] == '/') {
            sleep(1); //in reset sate, wait connect
            if (xhci_usb3_to_usb2_cause_syspatch_chage && access(module_sys_path, R_OK) && errno == ENOENT) {
                auto_find_quectel_modules(module_sys_path, MAX_PATH);
            }
            continue;
        }

        if (interfaceNum == 1)
            break;

        switch_to_edl_mode(usb_handle);
        qusb_noblock_close(usb_handle);
        usb_handle = NULL;
        sleep(1); //wait usb disconnect and re-connect
    }

    if (usb_handle == NULL) {
        error_return();
    }

    if (usb2tcp_port) {
        retval = usb2tcp_main(usb_handle, usb2tcp_port, qusb_zlp_mode);
        qusb_noblock_close(usb_handle);
        return retval;
    }

    start = get_now();
    retval = sahara_main(firehose_dir, usb_handle, idVendor == 0x05c6);

    if (!retval) {
        if (idVendor != 0x05C6) {
            sleep(1);
            stream_download(firehose_dir, usb_handle, qusb_zlp_mode);
            qusb_noblock_close(usb_handle);
            sleep(1);
            goto __edl_retry;
        }

__firehose_main:
        retval = firehose_main(firehose_dir, usb_handle, qusb_zlp_mode);
        if(retval == 0)
        {
            get_duration(start);
        }
    }

    qusb_noblock_close(usb_handle);

    if (firehose_dir) free(firehose_dir);
    if (module_port_name) free(module_port_name);
    if (module_sys_path) free(module_sys_path);

    dbg_time("Upgrade module %s.\n", retval == 0 ? "successfully" : "fail");    

    return retval;
}

double get_now()
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1000000;
}

void get_duration(double start)
{
    dbg_time("THE TOTAL DOWNLOAD TIME IS %.3f s\n",(get_now() - start));
}

void set_transfer_allbytes(long long bytes)
{
    transfer_bytes = 0;
    all_bytes_to_transfer = bytes;
}
/*
return percent
*/
int update_transfer_bytes(long long bytes_cur)
{
    static int last_percent = -1;
    int percent = 0;

    transfer_bytes += bytes_cur;
    percent = (transfer_bytes * 100) / all_bytes_to_transfer;
    if (percent != last_percent) {
        last_percent = percent;
        //printf("%lld - %lld \n", transfer_bytes, all_bytes_to_transfer);
        dbg_time("Upgrade progress:   %d\n", percent);
    }
    else {
        //printf("%d - %d\n", percent, last_percent);
    }

    return percent;
}
