#include <linux/module.h>
#include <linux/usb.h>
#include <linux/slab.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/delay.h>

MODULE_AUTHOR("DevTITANS <devtitans@icomp.ufam.edu.br>");
MODULE_DESCRIPTION("Driver de acesso ao SmartLamp (ESP32 com Chip Serial CP2102)");
MODULE_LICENSE("GPL");


#define MAX_RECV_LINE 100 // Tamanho máximo de uma linha de resposta do dispositivo USB

#define CP210X_IFC_ENABLE       0x00
#define CP210X_SET_LINE_CTL     0x03
#define CP210X_SET_MHS          0x07
#define CP210X_SET_FLOW         0x13
#define CP210X_SET_BAUDRATE     0x1e
#define CP210X_UART_ENABLE      0x0001
#define CP210X_LINE_CTL_8N1     0x0800
#define CP210X_CONTROL_DTR_RTS  0x0303
#define CP210X_DTR_ACTIVE       0x00000001
#define CP210X_RTS_ACTIVE       0x00000040

// struct cp210x_flow_ctl {
    // __le32 control_handshake;
    // __le32 flow_replace;
    // __le32 xon_limit;
    // __le32 xoff_limit;
// };

static char recv_line[MAX_RECV_LINE];              // Buffer para armazenar linha completa recebida
static struct usb_device *smartlamp_device;        // Referência para o dispositivo USB
static uint usb_in, usb_out;                       // Endereços das portas de entrada e saida da USB
static char *usb_in_buffer, *usb_out_buffer;       // Buffers de entrada e saída da USB
static int usb_max_size;                           // Tamanho máximo de uma mensagem USB

#define VENDOR_ID  0x10c4
#define PRODUCT_ID 0xea60
static const struct usb_device_id id_table[] = { { USB_DEVICE(VENDOR_ID, PRODUCT_ID) }, {} };

static int  usb_probe(struct usb_interface *ifce, const struct usb_device_id *id); // Executado quando o dispositivo é conectado na USB
static void usb_disconnect(struct usb_interface *ifce);                            // Executado quando o dispositivo USB é desconectado da USB
static int  usb_send_cmd(const char *cmd, int param);                              // Envia um comando e retorna a resposta

// Função para configurar os parâmetros seriais do CP2102 via Control-Messages
static int smartlamp_config_serial(struct usb_device *dev)
{
    int ret;
    u32 baudrate = 115200; // 115200 bps

    printk(KERN_INFO "SmartLamp: Configurando a porta serial...\n");

    // 1. Habilita a interface UART do CP2102
    //    Comando específico do vendor Silicon Labs (CP210X_IFC_ENABLE)
    //    bmRequestType: 0x41 (Vendor, Host-to-Device, Interface)
    //    bRequest: 0x00 (CP210X_IFC_ENABLE)
    //    wValue: 0x0001 (UART Enable)
    ret = usb_control_msg(dev, usb_sndctrlpipe(dev, 0),
                          CP210X_IFC_ENABLE, 0x41,
                          CP210X_UART_ENABLE, 0, NULL, 0, 1000);
    if (ret)
    {
        printk(KERN_ERR "SmartLamp: Erro ao habilitar a UART (código %d)\n", ret);
        return ret;
    }

    // 2. Define o baud rate
    //    Comando específico do vendor Silicon Labs (CP210X_SET_BAUDRATE)
    //    bRequest: 0x1E (CP210X_SET_BAUDRATE)
    ret = usb_control_msg_send(dev, 0, CP210X_SET_BAUDRATE, 0x41, 0, 0,
                               &baudrate, sizeof(baudrate), 1000, GFP_KERNEL);
    if (ret)
    {
        printk(KERN_ERR "SmartLamp: Erro ao configurar o baud rate (código %d)\n", ret);
        return ret;
    }

    // 3. Configura o formato utilizado pelo firmware: 8 bits, sem paridade,
    //    um stop bit (8N1).
    ret = usb_control_msg(dev, usb_sndctrlpipe(dev, 0),
                          CP210X_SET_LINE_CTL, 0x41,
                          CP210X_LINE_CTL_8N1, 0, NULL, 0, 1000);
    if (ret < 0) {
        printk(KERN_ERR "SmartLamp: Erro ao configurar 8N1 (%d)\n", ret);
        return ret;
    }

    /*
     * 4. Desativa controle de fluxo por hardware e software. O CP2102 mantém
     * essa configuração entre sessões, então não podemos depender do estado
     * deixado pelo Minicom ou pelo driver cp210x.
     */
    ret = usb_control_msg_send(dev, 0, CP210X_SET_FLOW, 0x41, 0, 0,
                               &flow, sizeof(flow), 1000, GFP_KERNEL);
    if (ret) {
        printk(KERN_WARNING "SmartLamp: CP2102 não aceitou controle de fluxo (%d); continuando\n", ret);
    }

    // 5. Ativa DTR e RTS, como o driver cp210x faz ao abrir a porta.
    ret = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), CP210X_SET_MHS, 0x41, CP210X_CONTROL_DTR_RTS, 0, NULL, 0, 1000);
    if (ret < 0) {
        printk(KERN_ERR "SmartLamp: Erro ao ativar DTR/RTS (%d)\n", ret);
        return ret;
    }

    msleep(1000);

    printk(KERN_INFO "SmartLamp: Serial configurada em 9600 8N1\n");
    return 0;
}

MODULE_DEVICE_TABLE(usb, id_table);
static struct usb_driver smartlamp_driver = {
    .name        = "smartlamp",     // Nome do driver
    .probe       = usb_probe,       // Executado quando o dispositivo é conectado na USB
    .disconnect  = usb_disconnect,  // Executado quando o dispositivo é desconectado na USB
    .id_table    = id_table,        // Tabela com o VendorID e ProductID do dispositivo
};

module_usb_driver(smartlamp_driver);

static int usb_probe(struct usb_interface *interface, const struct usb_device_id *id) {
    struct usb_endpoint_descriptor *usb_endpoint_in, *usb_endpoint_out;
    int ret;

    printk(KERN_INFO "SmartLamp: Dispositivo conectado ...\n");

    // Detecta portas e aloca buffers de entrada e saída de dados na USB
    smartlamp_device = interface_to_usbdev(interface);
    ret = usb_find_common_endpoints(interface->cur_altsetting, &usb_endpoint_in, &usb_endpoint_out, NULL, NULL);
    if (ret) { printk(KERN_ERR "SmartLamp: Endpoints bulk IN/OUT não encontrados (%d)\n", ret);
        return ret;
    }

    usb_max_size = usb_endpoint_maxp(usb_endpoint_in);
    usb_in = usb_endpoint_in->bEndpointAddress;
    usb_out = usb_endpoint_out->bEndpointAddress;
    usb_in_buffer = kmalloc(usb_max_size, GFP_KERNEL);
    usb_out_buffer = kmalloc(usb_max_size, GFP_KERNEL);
    if (!usb_in_buffer || !usb_out_buffer) {
        printk(KERN_ERR "SmartLamp: Falha ao alocar buffers USB\n");
        kfree(usb_in_buffer);
        kfree(usb_out_buffer);
        usb_in_buffer = NULL;
        usb_out_buffer = NULL;
        return -ENOMEM;
    }

    // Chama a função para configurar a porta serial antes de usar
    ret = smartlamp_config_serial(smartlamp_device);
    if (ret)
    {
        printk(KERN_ERR "SmartLamp: Falha na configuração da serial\n");
        kfree(usb_in_buffer);
        kfree(usb_out_buffer);
        return ret;
    }

    // Envia GET_LDR e aguarda a resposta "RES GET_LDR <valor>".
    ret = usb_send_cmd("GET_LDR", -1);
    if (ret < 0)
        goto error_free_buffers;

    printk(KERN_INFO "SmartLamp: Valor do LDR recebido: %d\n", ret);

    return 0;

error_free_buffers:
    kfree(usb_in_buffer);
    kfree(usb_out_buffer);
    usb_in_buffer = NULL;
    usb_out_buffer = NULL;
    smartlamp_device = NULL;
    return ret;
}

// Executado quando o dispositivo USB é desconectado da USB
static void usb_disconnect(struct usb_interface *interface) {
    printk(KERN_INFO "SmartLamp: Dispositivo desconectado.\n");
    kfree(usb_in_buffer);                   // Desaloca buffers
    kfree(usb_out_buffer);
    usb_in_buffer = NULL;
    usb_out_buffer = NULL;
    smartlamp_device = NULL;
}

// Envia um comando via USB, espera e retorna a resposta convertida para int.
// Exemplo de comando:  SET_LED 80
// Exemplo de resposta: RES SET_LED 1
static int usb_send_cmd(const char *cmd, int param)
{
    int recv_size = 0;
    int ret, actual_size, i, len;
    int retries = 10;
    int expected_len;
    char resp_expected[MAX_RECV_LINE];
    char *resp_pos;
    long resp_number;

    printk(KERN_INFO "SmartLamp: Enviando comando: %s\n", cmd);

    if (param >= 0)
        len = snprintf(usb_out_buffer, usb_max_size, "%s %d\n", cmd, param);
    else
        len = snprintf(usb_out_buffer, usb_max_size, "%s\n", cmd);

    if (len < 0 || len >= usb_max_size)
        return -EMSGSIZE;

    ret = usb_bulk_msg(smartlamp_device, usb_sndbulkpipe(smartlamp_device, usb_out), usb_out_buffer, len, &actual_size, 1000);
    if (ret) {
        printk(KERN_ERR "SmartLamp: Erro de código %d ao enviar comando\n", ret);
        return ret;
    }

    if (actual_size != len) {
        printk(KERN_ERR "SmartLamp: Envio incompleto (%d de %d bytes)\n", actual_size, len);
        return -EIO;
    }

    expected_len = snprintf(resp_expected, sizeof(resp_expected), "RES: %s", cmd);
    if (expected_len < 0 || expected_len >= sizeof(resp_expected))
        return -EMSGSIZE;

    printk(KERN_INFO "SmartLamp: Aguardando resposta '%s'\n", resp_expected);

    while (retries > 0) {
        ret = usb_bulk_msg(smartlamp_device,
                           usb_rcvbulkpipe(smartlamp_device, usb_in),
                           usb_in_buffer, usb_max_size,
                           &actual_size, 2000);
        if (ret) {
            retries--;
            printk(KERN_ERR
                   "SmartLamp: Erro ao ler USB; código %d (%d tentativas restantes)\n",
                   ret, retries);
            continue;
        }

        for (i = 0; i < actual_size; i++) {
            char c = usb_in_buffer[i];

            if (c == '\r')
                continue;

            if (c != '\n') {
                if (recv_size >= MAX_RECV_LINE - 1) {
                    printk(KERN_ERR "SmartLamp: Linha recebida muito grande\n");
                    return -EMSGSIZE;
                }

                recv_line[recv_size++] = c;
                continue;
            }

            if (recv_size == 0)
                continue;

            recv_line[recv_size] = '\0';
            printk(KERN_INFO "SmartLamp: Linha recebida: '%s'\n", recv_line);

            if (!strncmp(recv_line, resp_expected, expected_len) &&
                recv_line[expected_len] == ' ') {
                resp_pos = &recv_line[expected_len + 1];
                ret = kstrtol(resp_pos, 10, &resp_number);
                if (ret) {
                    printk(KERN_ERR
                           "SmartLamp: Valor inválido na resposta (%d)\n", ret);
                    return ret;
                }

                printk(KERN_INFO
                       "SmartLamp: Resposta de %s processada: %ld\n",
                       cmd, resp_number);
                return (int)resp_number;
            }

            if (!strncmp(recv_line, "ERR ", 4)) {
                printk(KERN_ERR "SmartLamp: Firmware retornou: %s\n",
                       recv_line);
                return -EPROTO;
            }

            retries--;
            printk(KERN_INFO
                   "SmartLamp: Linha não corresponde a %s (%d tentativas restantes)\n",
                   cmd, retries);
            recv_size = 0;
        }
    }

    printk(KERN_ERR "SmartLamp: Resposta de %s não recebida\n", cmd);
    return -ETIMEDOUT;
}
