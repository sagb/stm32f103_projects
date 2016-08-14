
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/usb/usbd.h>
#include <libopencm3/usb/cdc.h>

#include "usb_cdcacm.h"
#include "blink.h"
#include "../servo_pwm/servo.h"

#include "../../libs/microrl/microrl.h"


static const struct usb_device_descriptor dev = {
  .bLength = USB_DT_DEVICE_SIZE,
  .bDescriptorType = USB_DT_DEVICE,
  .bcdUSB = 0x0200,
  .bDeviceClass = USB_CLASS_CDC,
  .bDeviceSubClass = 0,
  .bDeviceProtocol = 0,
  .bMaxPacketSize0 = 64,
  .idVendor = 0x0483,
  .idProduct = 0x5740,
  .bcdDevice = 0x0200,
  .iManufacturer = 1,
  .iProduct = 2,
  .iSerialNumber = 3,
  .bNumConfigurations = 1,
};

/*
 * This notification endpoint isn't implemented. According to CDC spec it's
 * optional, but its absence causes a NULL pointer dereference in the
 * Linux cdc_acm driver.
 */
static const struct usb_endpoint_descriptor comm_endp[] = {{
  .bLength = USB_DT_ENDPOINT_SIZE,
  .bDescriptorType = USB_DT_ENDPOINT,
  .bEndpointAddress = 0x83,
  .bmAttributes = USB_ENDPOINT_ATTR_INTERRUPT,
  .wMaxPacketSize = 16,
  .bInterval = 255,
}};

static const struct usb_endpoint_descriptor data_endp[] = {{
  .bLength = USB_DT_ENDPOINT_SIZE,
  .bDescriptorType = USB_DT_ENDPOINT,
  .bEndpointAddress = 0x01,
  .bmAttributes = USB_ENDPOINT_ATTR_BULK,
  .wMaxPacketSize = 64,
  .bInterval = 1,
}, {
  .bLength = USB_DT_ENDPOINT_SIZE,
  .bDescriptorType = USB_DT_ENDPOINT,
  .bEndpointAddress = 0x82,
  .bmAttributes = USB_ENDPOINT_ATTR_BULK,
  .wMaxPacketSize = 64,
  .bInterval = 1,
}};

static const struct {
  struct usb_cdc_header_descriptor header;
  struct usb_cdc_call_management_descriptor call_mgmt;
  struct usb_cdc_acm_descriptor acm;
  struct usb_cdc_union_descriptor cdc_union;
} __attribute__((packed)) cdcacm_functional_descriptors = {
  .header = {
    .bFunctionLength = sizeof(struct usb_cdc_header_descriptor),
    .bDescriptorType = CS_INTERFACE,
    .bDescriptorSubtype = USB_CDC_TYPE_HEADER,
    .bcdCDC = 0x0110,
  },
  .call_mgmt = {
    .bFunctionLength =
      sizeof(struct usb_cdc_call_management_descriptor),
    .bDescriptorType = CS_INTERFACE,
    .bDescriptorSubtype = USB_CDC_TYPE_CALL_MANAGEMENT,
    .bmCapabilities = 0,
    .bDataInterface = 1,
  },
  .acm = {
    .bFunctionLength = sizeof(struct usb_cdc_acm_descriptor),
    .bDescriptorType = CS_INTERFACE,
    .bDescriptorSubtype = USB_CDC_TYPE_ACM,
    .bmCapabilities = 0,
  },
  .cdc_union = {
    .bFunctionLength = sizeof(struct usb_cdc_union_descriptor),
    .bDescriptorType = CS_INTERFACE,
    .bDescriptorSubtype = USB_CDC_TYPE_UNION,
    .bControlInterface = 0,
    .bSubordinateInterface0 = 1,
   }
};

static const struct usb_interface_descriptor comm_iface[] = {{
  .bLength = USB_DT_INTERFACE_SIZE,
  .bDescriptorType = USB_DT_INTERFACE,
  .bInterfaceNumber = 0,
  .bAlternateSetting = 0,
  .bNumEndpoints = 1,
  .bInterfaceClass = USB_CLASS_CDC,
  .bInterfaceSubClass = USB_CDC_SUBCLASS_ACM,
  .bInterfaceProtocol = USB_CDC_PROTOCOL_AT,
  .iInterface = 0,

  .endpoint = comm_endp,

  .extra = &cdcacm_functional_descriptors,
  .extralen = sizeof(cdcacm_functional_descriptors)
}};

static const struct usb_interface_descriptor data_iface[] = {{
  .bLength = USB_DT_INTERFACE_SIZE,
  .bDescriptorType = USB_DT_INTERFACE,
  .bInterfaceNumber = 1,
  .bAlternateSetting = 0,
  .bNumEndpoints = 2,
  .bInterfaceClass = USB_CLASS_DATA,
  .bInterfaceSubClass = 0,
  .bInterfaceProtocol = 0,
  .iInterface = 0,

  .endpoint = data_endp,
}};

static const struct usb_interface ifaces[] = {{
  .num_altsetting = 1,
  .altsetting = comm_iface,
}, {
  .num_altsetting = 1,
  .altsetting = data_iface,
}};

static const struct usb_config_descriptor config = {
  .bLength = USB_DT_CONFIGURATION_SIZE,
  .bDescriptorType = USB_DT_CONFIGURATION,
  .wTotalLength = 0,
  .bNumInterfaces = 2,
  .bConfigurationValue = 1,
  .iConfiguration = 0,
  .bmAttributes = 0x80,
  .bMaxPower = 0x32,

  .interface = ifaces,
};

static const char *usb_strings[] = {
  "Black Sphere Technologies",
  "CDC-ACM Demo",
  "DEMO",
};

/* Buffer to be used for control requests. */
uint8_t usbd_control_buffer[128];

static int cdcacm_control_request(usbd_device *usbd_dev, struct usb_setup_data *req, uint8_t **buf,
    uint16_t *len, void (**complete)(usbd_device *usbd_dev, struct usb_setup_data *req))
{
  (void)complete;
  (void)buf;
  (void)usbd_dev;

  switch (req->bRequest) {
  case USB_CDC_REQ_SET_CONTROL_LINE_STATE: {
    /*
     * This Linux cdc_acm driver requires this to be implemented
     * even though it's optional in the CDC spec, and we don't
     * advertise it in the ACM functional descriptor.
     */
    char local_buf[10];
    struct usb_cdc_notification *notif = (void*)local_buf;

    /* We echo signals back to host as notification. */
    notif->bmRequestType = 0xA1;
    notif->bNotification = USB_CDC_NOTIFY_SERIAL_STATE;
    notif->wValue = 0;
    notif->wIndex = 0;
    notif->wLength = 2;
    local_buf[8] = req->wValue & 3;
    local_buf[9] = 0;
    // usbd_ep_write_packet(0x83, buf, 10);
    return 1;
    }
  case USB_CDC_REQ_SET_LINE_CODING:
    if (*len < sizeof(struct usb_cdc_line_coding))
      return 0;

    return 1;
  }
  return 0;
}

//readline and usbdev ptr struct
typedef struct usb_command_line_s {
  microrl_t readline;
  usbd_device *usbd_dev;
} usb_command_line_t;



static usb_command_line_t usb_cmd_line;

static void usb_printn(const char *str, size_t len)
{
  while (usbd_ep_write_packet(usb_cmd_line.usbd_dev, 0x82, str, len) == 0);
}


static void print(const char *str)
{
  size_t len = strlen(str);  

  usb_printn(str, len);
}


static void usb_print(microrl_t *prl, const char *str)
{
  (void) prl;
  
  print(str);
}


static void cli_SET(int p1, int p2)
{
  char out[80];
  uint32_t real_pos = 0;
  /*outlen = */sprintf(out, "servo:%d value:%d\n", p1, p2);
  print(out);

  switch(p1){
     case 1:
       real_pos = servo_set_position(SERVO_CH1, p2); 
       break;
     case 2:
       real_pos = servo_set_position(SERVO_CH2, p2); 
       break;
     default:
       /*outlen = */sprintf(out, "WARN: servo number error. servo:%d value:%d\n", p1, p2);
       print(out); 
       break;
  }
  if(real_pos!=0){
    sprintf(out, "set %lu\n", real_pos);
    print(out);
  }
}


#include "ragel/servo_command_line.inc"

static void cdcacm_data_rx_cb(usbd_device *usbd_dev, uint8_t ep)
{
  (void)ep;
  (void)usbd_dev;
  int i;
  //int outlen;
  char buf[64];
  //char out[10];

  int len = usbd_ep_read_packet(usbd_dev, 0x01, buf, 64);

  if (len) {
    for(i=0;i<len;i++){
      //memset(out, 0, sizeof(out));
      //outlen = sprintf(out, "%d\n", (int)buf[i]);
      //while (usbd_ep_write_packet(usbd_dev, 0x82, out, outlen) == 0);
      microrl_insert_char (&usb_cmd_line.readline, buf[i]);
    } 
    //servo_set_position(SERVO_CH1, SERVO_NULL);
  }
  
}






static void execute_cmd_line(microrl_t *prl, int argc, const char * const * argv)
{
  (void) prl;
  (void) argc;
  (void) argv;
  int i;
  //gpio_toggle(GPIOC, GPIO13);
  for(i=0;i<argc;i++){
    if(argv[i]) {
      parse_stdin_command(argv[i], strlen(argv[i]));
    }
  }
}

static void cdcacm_set_config(usbd_device *usbd_dev, uint16_t wValue)
{
  (void)wValue;
  (void)usbd_dev;

  usbd_ep_setup(usbd_dev, 0x01, USB_ENDPOINT_ATTR_BULK, 64, cdcacm_data_rx_cb);
  usbd_ep_setup(usbd_dev, 0x82, USB_ENDPOINT_ATTR_BULK, 64, NULL);
  usbd_ep_setup(usbd_dev, 0x83, USB_ENDPOINT_ATTR_INTERRUPT, 16, NULL);

  usbd_register_control_callback(
        usbd_dev,
        USB_REQ_TYPE_CLASS | USB_REQ_TYPE_INTERFACE,
        USB_REQ_TYPE_TYPE | USB_REQ_TYPE_RECIPIENT,
        cdcacm_control_request);
}


void init_usb_cdcacm(void)
{
  microrl_init (&usb_cmd_line.readline, usb_print);
  microrl_set_execute_callback (&usb_cmd_line.readline, execute_cmd_line);

  usb_cmd_line.usbd_dev = usbd_init(&st_usbfs_v1_usb_driver, &dev, &config, usb_strings, 3, usbd_control_buffer, sizeof(usbd_control_buffer));
  usbd_register_set_config_callback(usb_cmd_line.usbd_dev, cdcacm_set_config);

}


void usb_poll_loop(void){
     while (1)
       usbd_poll(usb_cmd_line.usbd_dev);

}