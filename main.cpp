#include <unistd.h>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
#include <cstdarg>
#include <vector>
#include <filesystem>
#include <algorithm>
#include "config_file.h"
#include "PciDevice.h"

using std::string;
using std::vector;
namespace fs = std::filesystem;
typedef vector<uint32_t> intvec_t;

const uint32_t BC_EMU_RTL_ID = 912018;

struct global_t
{
    string   config_file = "bce_feeder.conf";
    string   pci_device;
    string   dir;
    int      max_repeats = 1;
    
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

    // This is a list of data-files to use for frame-data
    vector<string> data_files;

    // This is a vector of "vectors of 32-bit intgers".  Each integer
    // vector represents the frame data for a single bright-cycle
    vector<intvec_t> frame_data;

} g;

// This provides memory read/write access to the PCI device we care about
PciDevice device;

// Forward declarations
void execute(int argc, const char** argv);
void read_frame_data_files();
vector<string> get_file_list_from_directory(std::string directory);
bool start_fifo(uint32_t which);

//=============================================================================
// This routine isn't really a part of the program.  It exists to provide
// a convenient way to make some sample data-files
//=============================================================================
void generate_data_files()
{
    char filename[50];

    for (int file = 0; file<10; ++file)
    {
        sprintf(filename, "frame_data_%02i.csv", file);
        FILE* ofile = fopen(filename, "w");
        for (int entry = 0; entry<4297; ++entry)
        {
            fprintf(ofile, "0x%08X\n", entry | (file << 24));
        }
        fclose(ofile);
    }

    exit(0);
}
//=============================================================================



//=============================================================================
// main() just called "execute()" and handles exceptions
//=============================================================================
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

        if (token == "-dir" && argv[i])
        {
            g.dir = argv[i++];
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
    CConfigScript s;
    
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

    // If "data_files" exists in the configuration file, fetch a list 
    // of data-files to use as frame-data
    if (cf.exists("data_files"))
    {
        cf.get("data_files", &s);
        while (s.get_next_line())
        {
            auto filename = s.get_next_token();
            g.data_files.push_back(filename);
        }
    }

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

    // If the user gave us a directory name, fetch the filenames from it
    if (!g.dir.empty())
    {
        auto v = get_file_list_from_directory(g.dir);
        g.data_files = v;
    }

    // If the user hasn't specified any data files, complain
    if (g.data_files.empty()) throwRuntime("No data-files specified");

    // Read and parse the frame-data files into g.frame_data
    read_frame_data_files();

    // Reset the BC_EMU FIFOs
    *g.reg_fifo_ctl = 3;
    while (true)
    {
        usleep(1000);
        if (*g.reg_fifo_ctl == 0) break;
    }

    // Place BC_EMU into continuous mode
    *g.reg_cont_mode = 1;

    // Sending data frames to alternating FIFOs
    uint32_t which_fifo = 0;
    while (start_fifo(which_fifo))
    {
        which_fifo = 1 - which_fifo;
    }
   
}
//=============================================================================




//=============================================================================
// is_ws() - Returns true if the character pointed to is a space or tab
//=============================================================================
static bool is_ws(const char* p) {return ((*p == 32) || (*p == 9));}
//=============================================================================


//=============================================================================
// is_eol() - Returns true if the character pointed to is an end-of-line chr
//=============================================================================
static bool is_eol(const char* p)
{
    return ((*p == 10) || (*p == 13) || (*p == 0));
}
//=============================================================================


//=============================================================================
// skip_comma() - On return, the return value points to either an end-of-line 
//                character, or to the character immediately after a comma
//=============================================================================
static const char* skip_comma(const char* p)
{
    while (true)
    {
        if (*p == ',') return p+1;
        if (is_eol(p)) return p;
        ++p;
    }
}
//=============================================================================



//=============================================================================
// readMtVector() - Reads a CSV file full of integers and returns a vector
//                  containing them.  Values in file can be in hex or decimal,
//                  and can be comma separated into lines of arbitrary length. 
//                  File can contain blank lines and comment lines beginning
//                  with either "#" or "//"
//
// Will throw std::runtime error if file doesn't exist
//=============================================================================
intvec_t read_mt_vector(std::string filename)
{
    char buffer[0x10000];
    intvec_t result;

    printf("Reading %s\n", filename.c_str());

    // Try to open the input file
    FILE* ifile = fopen(filename.c_str(), "r");

    // Complain if we can't
    if (ifile == NULL) throwRuntime("can't read %s", filename.c_str());

    // Loop through each line of the file
    while (fgets(buffer, sizeof buffer, ifile))
    {
        // Point to the first byte of the buffer
        const char* p = buffer;

        // Skip over any leading whitespace
        while (is_ws(p)) ++p;

        // If the line is a "//" comment, skip it
        if (p[0] == '/' && p[1] == '/') continue;

        // If the line is a '#' comment, skip it
        if (*p == '#') continue;

        // This loop parses out comma-separated fields
        while (true)
        {
            // Skip over leading whitespace
            while (is_ws(p)) ++p;

            // If we've found the end of the line, we're done
            if (is_eol(p)) break;

            // Extract this value from the string
            uint32_t value = strtoul(p, nullptr, 0);

            // Append it to our result vector
            result.push_back(value);
        
            // Point to the next field
            p = skip_comma(p);
        }
    }

    // Close the input file, we're done reading it
    fclose(ifile);

    // Hand the resulting vector to the caller
    return result;
}
//=============================================================================


//=============================================================================
// This reads in all of the files specified by g.data_file.  Each file is
// parsed into a vector of integers, and that vector is appended to the
// structure g.frame_data
//=============================================================================
void read_frame_data_files()
{
    for (auto filename : g.data_files)
    {
        intvec_t v = read_mt_vector(filename);
        g.frame_data.push_back(v);
    }
}
//=============================================================================




//=============================================================================
// This returns a list containing the name of every .csv file in the specified
// directory.   The returned list is sorted alphabetically
//=============================================================================
vector<string> get_file_list_from_directory(std::string directory)
{
    vector<string> result;

    for (const auto & entry : fs::directory_iterator(directory))
    {
        // Get the extension of this directory entry
        auto extent = entry.path().extension();

        // If this is a .csv file, add it to the result list
        if (fs::is_regular_file(entry.status()) && extent == ".csv")
        {
            result.push_back(entry.path());
        }
    }

    // Sort the result list
    std::sort(result.begin(), result.end());

    // Hand the resulting, sorted list to the caller
    return result;
}
//=============================================================================



//=============================================================================
// Returns the index (within g.frame_data) of the next vector of frame data
// to load into a fifo.
//=============================================================================
int get_next_frame_index()
{
    static int current_frame_index = 0;
    static int current_repeat = 0;

    // If this is the first time we've been called...
    if (current_repeat == 0)
    {
        current_repeat = 1;
        return current_frame_index;
    }

    // We are either going to repeat this frame, or we
    // need to increment to a new frame
    if (current_repeat < g.max_repeats)
        ++current_repeat;
    else
    {
        current_repeat == 1;
        ++current_frame_index;
    }

    if (current_frame_index < g.frame_data.size())
        return current_frame_index;

    // If we get here, there are no more frames of data
    // available to send
    return -1;
}
//=============================================================================



//=============================================================================
// This loads a FIFO, tells the RTL to start sending frames using the data
// from that FIFO, and waits for the RTL to report that it has begun doing so
//=============================================================================
bool start_fifo(uint32_t which)
{
    // A pointer to the FIFO register
    volatile uint32_t* fifo;
    
    // This will have a 1 in bit 0 or in bit 1
    uint32_t           fifo_bit;

    // Determine the runtime parameters for this particular FIFO
    if (which == 0)
    {
        fifo = g.reg_fifo0;
        fifo_bit = 1 << 0;
    }
    else
    {
        fifo = g.reg_fifo1;
        fifo_bit = 1 << 1;
    }

    // Reset the FIFO (i.e., remove any existing entries)
    *g.reg_fifo_ctl = fifo_bit;
    while (*g.reg_fifo_ctl) usleep(100);

    // Find the index of the frame data we should load into the FIFO
    int index = get_next_frame_index();

    // If we have frame-data to load into the FIFO...
    if (index >= 0)
    {
        printf("Loading frame %i into FIFO_%i...", index, which);
        fflush(stdout);
        // Load the frame data into the FIFO
        intvec_t& frame_data = g.frame_data[index];
        for (uint32_t v : frame_data)
        {
            *fifo = v;
            usleep(50);
        }

        // Tell the RTL to put this FIFO "on deck"
        *g.reg_fifo_select = fifo_bit;

        // Wait for the RTL to make this FIFO active
        while (*g.reg_fifo_select != fifo_bit) usleep(1000);

        printf("started\n");

        // And tell the caller that his FIFO is loaded and active
        return true;
    }

    printf("Stopping job\n");

    // If we get here, we have no more frame-data to send and are
    // stopping the job
    *g.reg_fifo_select = 0;
    while (*g.reg_fifo_select) usleep(1000);

    printf("Job complete\n");

    // Tell the caller that the job is complete
    return false;
}
//=============================================================================