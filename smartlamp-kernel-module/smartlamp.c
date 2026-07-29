#include <linux/module.h>
#include <linux/usb.h>
#include <linux/slab.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/mutex.h>
#include <linux/string.h>
#include <linux/delay.h>

MODULE_AUTHOR("DevTITANS <devtitans@icomp.ufam.edu.br>");
MODULE_DESCRIPTION("Driver de acesso ao SmartLamp (ESP32 com Chip Serial CP2102)");
MODULE_LICENSE("GPL");

#define MAX_RECV_LINE 100
#define VENDOR_ID     0x10c4
#define PRODUCT_ID    0xea60

#define CP210X_IFC_ENABLE    0x00
#define CP210X_SET_LINE_CTL  0x03
#define CP210X_SET_BAUDRATE  0x1e
#define CP210X_UART_ENABLE   0x0001
#define CP210X_LINE_CTL_8N1  0x0800

static char recv_line[MAX_RECV_LINE];
static struct usb_device *smartlamp_device;
static uint usb_in;
static uint usb_out;
static char *usb_in_buffer;
static char *usb_out_buffer;
static int usb_in_size;
static int usb_out_size;
static struct kobject *sys_obj;

/*
 * Garante que apenas um acesso ao sysfs use os buffers e a comunicação
 * comando/resposta de cada vez.
 */
static DEFINE_MUTEX(io_lock);

static int usb_probe(struct usb_interface *ifce,
                     const struct usb_device_id *id);
static void usb_disconnect(struct usb_interface *ifce);
static int usb_send_cmd(const char *cmd, int param);

static ssize_t attr_show(struct kobject *sys_obj,
                         struct kobj_attribute *attr, char *buff);
static ssize_t attr_store(struct kobject *sys_obj,
                          struct kobj_attribute *attr,
                          const char *buff, size_t count);

static struct kobj_attribute led_attribute =
    __ATTR(led, 0644, attr_show, attr_store);
static struct kobj_attribute ldr_attribute =
    __ATTR(ldr, 0644, attr_show, attr_store);

static struct attribute *attrs[] = {
    &led_attribute.attr,
    &ldr_attribute.attr,
    NULL,
};

static const struct attribute_group attr_group = {
    .attrs = attrs,
};

static const struct usb_device_id id_table[] = {
    { USB_DEVICE(VENDOR_ID, PRODUCT_ID) },
    { }
};
MODULE_DEVICE_TABLE(usb, id_table);

static struct usb_driver smartlamp_driver = {
    .name = "smartlamp",
    .probe = usb_probe,
    .disconnect = usb_disconnect,
    .id_table = id_table,
};
module_usb_driver(smartlamp_driver);

/* Configura o CP2102 com os mesmos parâmetros usados pelo SmartLamp.ino. */
static int smartlamp_config_serial(struct usb_device *dev)
{
    __le32 baudrate = cpu_to_le32(115200);
    int ret;

    printk(KERN_INFO "SmartLamp: Configurando a porta serial...\n");

    ret = usb_control_msg(dev, usb_sndctrlpipe(dev, 0),
                          CP210X_IFC_ENABLE, 0x41,
                          CP210X_UART_ENABLE, 0,
                          NULL, 0, 1000);
    if (ret) {
        printk(KERN_ERR "SmartLamp: Erro ao habilitar UART (%d)\n", ret);
        return ret;
    }

    ret = usb_control_msg_send(dev, 0, CP210X_SET_BAUDRATE, 0x41,
                               0, 0, &baudrate, sizeof(baudrate),
                               1000, GFP_KERNEL);
    if (ret) {
        printk(KERN_ERR "SmartLamp: Erro ao configurar baud rate (%d)\n",
               ret);
        return ret;
    }

    ret = usb_control_msg(dev, usb_sndctrlpipe(dev, 0),
                          CP210X_SET_LINE_CTL, 0x41,
                          CP210X_LINE_CTL_8N1, 0,
                          NULL, 0, 1000);
    if (ret < 0) {
        printk(KERN_ERR "SmartLamp: Erro ao configurar 8N1 (%d)\n", ret);
        return ret;
    }

    msleep(100);
    printk(KERN_INFO "SmartLamp: Serial configurada em 115200 8N1\n");
    return 0;
}

static int usb_probe(struct usb_interface *interface,
                     const struct usb_device_id *id)
{
    struct usb_endpoint_descriptor *endpoint_in;
    struct usb_endpoint_descriptor *endpoint_out;
    int ret;

    printk(KERN_INFO "SmartLamp: Dispositivo conectado\n");

    smartlamp_device = interface_to_usbdev(interface);

    ret = usb_find_common_endpoints(interface->cur_altsetting,
                                    &endpoint_in, &endpoint_out,
                                    NULL, NULL);
    if (ret) {
        printk(KERN_ERR "SmartLamp: Endpoints bulk não encontrados (%d)\n",
               ret);
        smartlamp_device = NULL;
        return ret;
    }

    usb_in = endpoint_in->bEndpointAddress;
    usb_out = endpoint_out->bEndpointAddress;
    usb_in_size = usb_endpoint_maxp(endpoint_in);
    usb_out_size = usb_endpoint_maxp(endpoint_out);

    usb_in_buffer = kmalloc(usb_in_size, GFP_KERNEL);
    usb_out_buffer = kmalloc(usb_out_size, GFP_KERNEL);
    if (!usb_in_buffer || !usb_out_buffer) {
        ret = -ENOMEM;
        goto error_free_buffers;
    }

    ret = smartlamp_config_serial(smartlamp_device);
    if (ret)
        goto error_free_buffers;

    sys_obj = kobject_create_and_add("smartlamp", kernel_kobj);
    if (!sys_obj) {
        ret = -ENOMEM;
        goto error_free_buffers;
    }

    ret = sysfs_create_group(sys_obj, &attr_group);
    if (ret) {
        printk(KERN_ERR "SmartLamp: Erro ao criar arquivos sysfs (%d)\n",
               ret);
        kobject_put(sys_obj);
        sys_obj = NULL;
        goto error_free_buffers;
    }

    printk(KERN_INFO
           "SmartLamp: Arquivos /sys/kernel/smartlamp/{led,ldr} criados\n");
    return 0;

error_free_buffers:
    kfree(usb_in_buffer);
    kfree(usb_out_buffer);
    usb_in_buffer = NULL;
    usb_out_buffer = NULL;
    smartlamp_device = NULL;
    return ret;
}

static void usb_disconnect(struct usb_interface *interface)
{
    printk(KERN_INFO "SmartLamp: Dispositivo desconectado\n");

    if (sys_obj) {
        sysfs_remove_group(sys_obj, &attr_group);
        kobject_put(sys_obj);
        sys_obj = NULL;
    }

    mutex_lock(&io_lock);
    smartlamp_device = NULL;
    kfree(usb_in_buffer);
    kfree(usb_out_buffer);
    usb_in_buffer = NULL;
    usb_out_buffer = NULL;
    mutex_unlock(&io_lock);
}

/*
 * Envia um comando, aguarda "RES <cmd> <numero>" e retorna o número.
 * param < 0 envia apenas o comando, sem parâmetro.
 */
static int usb_send_cmd(const char *cmd, int param)
{
    char resp_expected[MAX_RECV_LINE];
    char *resp_pos;
    long resp_number;
    int expected_len;
    int recv_size = 0;
    int retries = 10;
    int actual_size;
    int len;
    int ret;
    int i;

    ret = mutex_lock_interruptible(&io_lock);
    if (ret)
        return ret;

    if (!smartlamp_device || !usb_in_buffer || !usb_out_buffer) {
        ret = -ENODEV;
        goto out_unlock;
    }

    if (param >= 0)
        len = snprintf(usb_out_buffer, usb_out_size,
                       "%s %d\n", cmd, param);
    else
        len = snprintf(usb_out_buffer, usb_out_size, "%s\n", cmd);

    if (len < 0 || len >= usb_out_size) {
        ret = -EMSGSIZE;
        goto out_unlock;
    }

    printk(KERN_INFO "SmartLamp: Enviando comando: %s", usb_out_buffer);

    ret = usb_bulk_msg(smartlamp_device,
                       usb_sndbulkpipe(smartlamp_device, usb_out),
                       usb_out_buffer, len, &actual_size, 1000);
    if (ret) {
        printk(KERN_ERR "SmartLamp: Erro ao enviar comando (%d)\n", ret);
        goto out_unlock;
    }

    if (actual_size != len) {
        printk(KERN_ERR "SmartLamp: Envio incompleto (%d de %d bytes)\n",
               actual_size, len);
        ret = -EIO;
        goto out_unlock;
    }

    expected_len = snprintf(resp_expected, sizeof(resp_expected),
                            "RES %s", cmd);
    if (expected_len < 0 || expected_len >= sizeof(resp_expected)) {
        ret = -EMSGSIZE;
        goto out_unlock;
    }

    while (retries > 0) {
        ret = usb_bulk_msg(smartlamp_device,
                           usb_rcvbulkpipe(smartlamp_device, usb_in),
                           usb_in_buffer, usb_in_size,
                           &actual_size, 2000);
        if (ret) {
            retries--;
            printk(KERN_WARNING
                   "SmartLamp: Erro de leitura %d; %d tentativas restantes\n",
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
                    ret = -EMSGSIZE;
                    goto out_unlock;
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
                           "SmartLamp: Número inválido na resposta (%d)\n",
                           ret);
                    goto out_unlock;
                }

                ret = (int)resp_number;
                goto out_unlock;
            }

            if (!strncmp(recv_line, "ERR ", 4)) {
                printk(KERN_ERR "SmartLamp: Firmware retornou: %s\n",
                       recv_line);
                ret = -EPROTO;
                goto out_unlock;
            }

            recv_size = 0;
            retries--;
        }
    }

    printk(KERN_ERR "SmartLamp: Resposta para %s não recebida\n", cmd);
    ret = -ETIMEDOUT;

out_unlock:
    mutex_unlock(&io_lock);
    return ret;
}

static ssize_t attr_show(struct kobject *object,
                         struct kobj_attribute *attr, char *buff)
{
    const char *attr_name = attr->attr.name;
    int value;

    printk(KERN_INFO "SmartLamp: Lendo %s\n", attr_name);

    if (!strcmp(attr_name, "led"))
        value = usb_send_cmd("GET_LED", -1);
    else if (!strcmp(attr_name, "ldr"))
        value = usb_send_cmd("GET_LDR", -1);
    else
        return -EINVAL;

    if (value < 0)
        return value;

    return sysfs_emit(buff, "%d\n", value);
}

static ssize_t attr_store(struct kobject *object,
                          struct kobj_attribute *attr,
                          const char *buff, size_t count)
{
    const char *attr_name = attr->attr.name;
    long value;
    int ret;

    if (!strcmp(attr_name, "ldr")) {
        printk(KERN_ERR "SmartLamp: ldr é somente leitura\n");
        return -1;
    }

    if (strcmp(attr_name, "led"))
        return -EINVAL;

    ret = kstrtol(buff, 10, &value);
    if (ret) {
        printk(KERN_ERR "SmartLamp: Valor de led inválido\n");
        return ret;
    }

    if (value < 0 || value > 100) {
        printk(KERN_ERR "SmartLamp: LED deve estar entre 0 e 100\n");
        return -ERANGE;
    }

    ret = usb_send_cmd("SET_LED", (int)value);
    if (ret != 1) {
        printk(KERN_ERR "SmartLamp: Erro ao definir LED (resposta %d)\n",
               ret);
        return ret < 0 ? ret : -EIO;
    }

    return count;
}
