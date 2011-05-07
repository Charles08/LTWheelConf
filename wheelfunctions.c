/*
 *    ltwheelconf - configure logitech racing wheels
 * 
 *    Copyright (C) 2011  Michael Bauer <michael@m-bauer.org>
 * 
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 * 
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 * 
 *    You should have received a copy of the GNU General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 * 
 */

#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

#include <linux/input.h>

#include "wheels.h"
#include "wheelfunctions.h"

#define TRANSFER_WAIT_TIMEOUT_MS 5000
#define CONFIGURE_WAIT_SEC 3
#define UDEV_WAIT_SEC 2

/* Globals */
extern int verbose_flag;


void list_devices() {
    libusb_device_handle *handle = 0;
    libusb_device *dev = 0;
    struct libusb_device_descriptor desc;
    unsigned char descString[255];
    memset(&descString, 0, sizeof(descString));

    int numFound = 0;
    int PIDIndex = 0;
    wheelstruct w = wheels[PIDIndex];
    for (PIDIndex = 0; PIDIndex < PIDs_END; PIDIndex++) {
        w = wheels[PIDIndex];
        printf("Scanning for \"%s\": ", w.name);
        handle = libusb_open_device_with_vid_pid(NULL, VID_LOGITECH, w.native_pid);
        if (handle != 0) {
            dev = libusb_get_device(handle);
            if (dev != 0) {
                int ret = libusb_get_device_descriptor(dev, &desc);
                if (ret == 0) {
                    numFound++;
                    libusb_get_string_descriptor_ascii(handle, desc.iProduct, descString, 255);
                    printf("\t\tFound \"%s\", %04x:%04x (bus %d, device %d)", descString, desc.idVendor, desc.idProduct,
                           libusb_get_bus_number(dev), libusb_get_device_address(dev));
                    
                } else {
                    perror("Get device descriptor");
                }
            } else {
                perror ("Get device");
            }
        }
        printf("\n");
    }
    printf("Found %d devices.\n", numFound);
}


int send_command(libusb_device_handle *handle, cmdstruct command ) {
    if (command.numCmds == 0) {
        printf( "send_command: Empty command provided! Not sending anything...\n");
        return 0;
    }

    int stat;
    stat = libusb_detach_kernel_driver(handle, 0);
    if ((stat < 0 ) || verbose_flag) perror("Detach kernel driver");

    stat = libusb_claim_interface( handle, 0 );
    if ( (stat < 0) || verbose_flag) perror("Claiming USB interface");

    int transferred = 0;
    
    // send all command strings provided in command
    int cmdCount;
    for (cmdCount=0; cmdCount < command.numCmds; cmdCount++) {
        stat = libusb_interrupt_transfer( handle, 1, command.cmds[cmdCount], sizeof( command.cmds[cmdCount] ), &transferred, TRANSFER_WAIT_TIMEOUT_MS );
        if ( (stat < 0) || verbose_flag) perror("Sending USB command");
    }

    /* In case the command just sent caused the device to switch from restricted mode to native mode
     * the following two commands will fail due to invalid device handle (because the device changed
     * its pid on the USB bus). 
     * So it is not possible anymore to release the interface and re-attach kernel driver.
     * I am not sure if this produces a memory leak within libusb, but i do not think there is another 
     * solution possible...
     */
    stat = libusb_release_interface(handle, 0 );
    if (stat != LIBUSB_ERROR_NO_DEVICE) { // silently ignore "No such device" error due to reasons explained above.
        if ( (stat < 0) || verbose_flag) {
            perror("Releasing USB interface.");
        }
    }

    stat = libusb_attach_kernel_driver( handle, 0);
    if (stat != LIBUSB_ERROR_NO_DEVICE) { // silently ignore "No such device" error due to reasons explained above.
        if ( (stat < 0) || verbose_flag) {
            perror("Reattaching kernel driver");
        }
    }
    return 0;
}

int set_native_mode(int wheelIndex) {
    wheelstruct w = wheels[wheelIndex];

    // first check if wheel has native mode at all
    if (w.native_pid == w.restricted_pid) {
        printf( "%s is always in native mode.\n", w.name);
        return 0;
    }
    
    // check if wheel is already in native mode
    libusb_device_handle *handle = libusb_open_device_with_vid_pid(NULL, VID_LOGITECH, w.native_pid);
    if ( handle != NULL ) {
        printf( "Found a %s already in native mode.\n", w.name);
        return 0;
    }
    
    // check if we know how to set native mode
    if (!w.cmd_native.numCmds) {
        printf( "Sorry, do not know how to set %s into native mode.\n", w.name);
        return -1;
    }
    
    // try to get handle to device in restricted mode
    handle = libusb_open_device_with_vid_pid(NULL, VID_LOGITECH, w.restricted_pid );
    if ( handle == NULL ) {
        printf( "Can not find %s in restricted mode (PID %x). This should not happen :-(\n", w.name, w.restricted_pid);
        return -1;
    }

    // finally send command to switch wheel to native mode
    send_command(handle, w.cmd_native);

    // wait until wheel reconfigures to new PID...
    sleep(CONFIGURE_WAIT_SEC);
    
    // If above command was successfully we should now find the wheel in extended mode
    handle = libusb_open_device_with_vid_pid(NULL, VID_LOGITECH, w.native_pid);
    if ( handle != NULL ) {
        printf ( "%s is now set to native mode.\n", w.name);
    } else {
        // this should not happen, just in case
        printf ( "Unable to set %s to native mode.\n", w.name );
        return -1;
    }

    return 0;
}
    

int set_range(int wheelIndex, unsigned short int range) {
    wheelstruct w = wheels[wheelIndex];
    
    libusb_device_handle *handle = libusb_open_device_with_vid_pid(NULL, VID_LOGITECH, w.native_pid );
    if ( handle == NULL ) {
        printf ( "%s not found. Make sure it is set to native mode (use --native).\n", w.name);
        return -1;
    }
    
    // Build up command to set range.
    
    // check if we know how to set range
    if (!w.cmd_range_prefix) {
        printf( "Sorry, do not know how to set rotation range for %s.\n", w.name);
        return -1;
    }
    
    // FIXME - This cmd_range_prefix stuff is really ugly for now...
    cmdstruct setrange = {
        { 
            { w.cmd_range_prefix[0], w.cmd_range_prefix[1], range & 0x00ff , (range & 0xff00)>>8, 0x00, 0x00, 0x00 }
        },
        1,
    };
    // send command to change range
    send_command(handle, setrange);
    printf ("Wheel rotation range of %s is now set to %d degrees.\n", w.name, range);
    return 0;
    
}


int set_autocenter(int wheelIndex, int centerforce, int rampspeed)
{
    if (verbose_flag) printf ( "Setting autocenter...");
    
    wheelstruct w = wheels[wheelIndex];
    
    libusb_device_handle *handle = libusb_open_device_with_vid_pid(NULL, VID_LOGITECH, w.native_pid );
    if ( handle == NULL ) {
        printf ( "%s not found. Make sure it is set to native mode (use --native).\n", w.name);
        return -1;
    }
    
    // Build up command to set range.
    
    // check if we know how to set native range
    if (!w.cmd_autocenter_prefix) {
        printf( "Sorry, do not know how to set autocenter force for %s. Please try generic implementation using --alt_autocenter.\n", w.name);
        return -1;
    }
    
    // FIXME - This cmd_autocenter_prefix stuff is really ugly for now...
    cmdstruct command = {
        { 
            { w.cmd_autocenter_prefix[0], w.cmd_autocenter_prefix[1], rampspeed & 0x0f , rampspeed & 0x0f, centerforce & 0xff, 0x00, 0x00, 0x00 }
        },
        1,
    };
    
    send_command(handle, command);
    
    printf ("Autocenter for %s is now set to %d with rampspeed %d.\n", w.name, centerforce, rampspeed);
    return 0;
}

int alt_set_autocenter(int centerforce, char *device_file_name, int wait_for_udev) {
    if (verbose_flag) printf ( "Device %s: Setting autocenter force to %d.\n", device_file_name, centerforce );
    
    /* sleep UDEV_WAIT_SEC seconds to allow udev to set up device nodes due to kernel 
     * driver re-attaching while setting native mode or wheel range before
     */
    if (wait_for_udev) sleep(UDEV_WAIT_SEC);

    /* Open device */
    int fd = open(device_file_name, O_RDWR);
    if (fd == -1) {
        perror("Open device file");
        return -1;
    }

    if (centerforce >= 0 && centerforce <= 100) {
        struct input_event ie;
        ie.type = EV_FF;
        ie.code = FF_AUTOCENTER;
        ie.value = 0xFFFFUL * centerforce/100;
        if (write(fd, &ie, sizeof(ie)) == -1) {
            perror("set auto-center");
            return -1;
        }
    }
    printf ("Wheel autocenter force is now set to %d.\n", centerforce);
    return 0;
}


int set_gain(int gain, char *device_file_name, int wait_for_udev) {
    if (verbose_flag) printf ( "Device %s: Setting FF gain to %d.\n", device_file_name, gain);
    
    /* sleep UDEV_WAIT_SEC seconds to allow udev to set up device nodes due to kernel 
     * driver re-attaching while setting native mode or wheel range before
     */
    if (wait_for_udev) sleep(UDEV_WAIT_SEC);
    
    /* Open device */
    int fd = open(device_file_name, O_RDWR);
    if (fd == -1) {
        perror("Open device file");
        return -1;
    }
    
    if (gain >= 0 && gain <= 100) {
        struct input_event ie;
        ie.type = EV_FF;
        ie.code = FF_GAIN;
        ie.value = 0xFFFFUL * gain / 100;
        if (write(fd, &ie, sizeof(ie)) == -1) {
            perror("set gain");
            return -1;
        }
    }
    printf ("Wheel forcefeedback gain is now set to %d.\n", gain);
    return 0;
}


