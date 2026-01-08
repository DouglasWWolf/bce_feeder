#include <unistd.h>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
#include <cstdarg>
#include "config_file.h"
#include "PciDevice.h"

using std::string;

const uint32_t BC_EMU_RTL_ID = 912018;

struct global_t
{
    string   config_file = "bce_feeder.conf";
    string   pci_device;
    
    // Offsets to the BC_EMU registers
    uint32_t reg_rtl_id_offset;
    uint32_t reg_fifo0_offset;
    uint32_t reg_fifo1_offset;
    uint32_t reg_fifo_ctl_offset;
    uint32_t reg_fifo_select_offset;
    uint32_t reg_cont_mode_offset;
    uint32_t reg_nshot_limit_offset;

    // Pointers (in userspace) to the BC_EMU registers
    volatile uint32_t* reg_rtl_id;
    volatile uint32_t* reg_fifo0;
    volatile uint32_t* reg_fifo1;
    volatile uint32_t* reg_fifo_ctl;
    volatile uint32_t* reg_fifo_select;
    volatile uint32_t* reg_cont_mode;
    volatile uint32_t* reg_nshot_limit;

} g;

// This provides memory read/write access to the PCI device we care about
PciDevice device;

void execute(int argc, const char** argv);

int main(int argc, const char** argv)
{
    try
    {
        execute(argc, argv);
    }
    catch(const std::exception& e)
    {
        fprintf(stderr, "%s\n", e.what());
        exit(1);
    }
}



//=============================================================================
// throwRuntime() - Throws a runtime exception
//=============================================================================
static void throwRuntime(const char* fmt, ...)
{
    char buffer[1024];
    va_list ap;
    va_start(ap, fmt);
    vsprintf(buffer, fmt, ap);
    va_end(ap);

    throw std::runtime_error(buffer);
}
//=============================================================================




//=============================================================================
// parse_command_line() - Parse the command line options and fill in the 
//                        corresponding global variables
//=============================================================================
void parse_command_line(const char** argv)
{
    int i=1;

    // Loop through all the command line arguments
    while (argv[i])
    {
        
        // Fetch the next token from the command line
        string token = argv[i++];

        // Is the user specifying the name of a config-file?
        if (token == "-config" && argv[i])
        {
            g.config_file = argv[i++];
            continue;
        }

        // If we get here, we've encountered a command-line option that
        // we don't recognize
        fprintf(stderr, "Invalid command line option %s\n", token.c_str());
        exit(1);
    }
}
//=============================================================================


//=============================================================================
// parse the configuration file
//=============================================================================
void parse_config_file(const string filename)
{
    CConfigFile cf;
    
    // Read the configuration file
    if (!cf.read(filename)) exit(1);

    // Fetch the VendorID:DeviceID of the PCI device we're interested in
    cf.get("pci_device", &g.pci_device);

    // Fetch the offsets of the registers we care about
    cf.get("reg_rtl_id",      &g.reg_rtl_id_offset     );
    cf.get("reg_fifo0",       &g.reg_fifo0_offset      );
    cf.get("reg_fifo1",       &g.reg_fifo1_offset      );
    cf.get("reg_fifo_ctl",    &g.reg_fifo_ctl_offset   );
    cf.get("reg_fifo_select", &g.reg_fifo_select_offset);
    cf.get("reg_cont_mode",   &g.reg_cont_mode_offset  );
    cf.get("reg_nshot_limit", &g.reg_nshot_limit_offset);
}
//=============================================================================



//=============================================================================
// This is the true top-level execution of this program
//=============================================================================
void execute(int argc, const char** argv)
{
    // Parse the command line options
    parse_command_line(argv);

    // Parse the configuration file
    parse_config_file(g.config_file);

    // Map the PCI-device's memory into userspace
    device.open(g.pci_device);

    // Fetch the userspace pointer to the device's first resource
    auto base_ptr = device.resourceList()[0].baseAddr;

    // Compute the addresses of the pointers to the BC_EMU registers
    g.reg_rtl_id      = (uint32_t*)(base_ptr + g.reg_rtl_id_offset);
    g.reg_fifo0       = (uint32_t*)(base_ptr + g.reg_fifo0_offset);
    g.reg_fifo1       = (uint32_t*)(base_ptr + g.reg_fifo1_offset);
    g.reg_fifo_ctl    = (uint32_t*)(base_ptr + g.reg_fifo_ctl_offset);
    g.reg_fifo_select = (uint32_t*)(base_ptr + g.reg_fifo_select_offset);
    g.reg_cont_mode   = (uint32_t*)(base_ptr + g.reg_cont_mode_offset);
    g.reg_nshot_limit = (uint32_t*)(base_ptr + g.reg_nshot_limit_offset);

    // Check to make sure that BC_EMU is actually loaded!
    if (*g.reg_rtl_id != BC_EMU_RTL_ID) throwRuntime("BC_EMU isn't loaded!");

    // Reset the BC_EMU FIFOs
    *g.reg_fifo_ctl = 3;
    while (true)
    {
        usleep(1000);
        if (*g.reg_fifo_ctl == 0) break;
    }

    // Place BC_EMU into nshot mode
    *g.reg_cont_mode = 0;
    
    // BC_EMU will make exactly 1 pass through each FIFO
    *g.reg_nshot_limit = 1;
}
//=============================================================================