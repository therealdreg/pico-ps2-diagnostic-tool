/*
MIT License - okhi - Open Keylogger Hardware Implant
---------------------------------------------------------------------------
Copyright (c) [2024] by David Reguera Garcia aka Dreg
https://github.com/therealdreg/pico_ps2_diagnostic_tool
https://www.rootkit.es
X @therealdreg
dreg@rootkit.es
---------------------------------------------------------------------------
Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
---------------------------------------------------------------------------
WARNING: BULLSHIT CODE X-)
---------------------------------------------------------------------------
*/

// This project assumes that copy_to_ram is enabled, so ALL code is running from RAM

/*
    Dreg's note: I have tried to document everything as best as possible and make the code and project
    as accessible as possible for beginners. There may be errors (I am a lazy bastard using COPILOT)
    if you find any, please make a PR

    I'm still a novice with the Pico SDK & RP2040, so please bear with me if there are unnecessary things ;-)
    --

    This program captures and replays signals on a PS/2 interface (DATA and CLOCK lines).
    A "capture" is a sequence of GPIO readings taken at short intervals, effectively logging
    the entire timeline of PS/2 pin states during recording. Each capture can be replayed
    (emulating the original signal), stored permanently in flash memory, deleted, or exported.
    Multiple captures can be managed in flash (record, play, delete, import, export),
    providing a versatile diagnostic tool for PS/2 devices or signal analysis. Additionally,
    a glitch detector monitors extremely short pulses on the PS/2 clock line.
*/

#include <stdio.h>
#include <string.h>

#include "hardware/clocks.h"
#include "hardware/flash.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/pll.h"
#include "hardware/structs/clocks.h"
#include "hardware/structs/pll.h"
#include "hardware/timer.h"
#include "hardware/watchdog.h"
#include "pico/bootrom.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "pico_ps2_diagnostic_tool.pio.h"
#include "tusb.h"

#define BP() __asm("bkpt #1"); // breakpoint via software macro

#define FVER 1

#define FLASH_CAPTURE_SECTOR_COUNT 0x190
#define MAX_CAPTURE_COUNT                                                                                              \
    ((((0x9FFF * sizeof(uint32_t)) + (FLASH_SECTOR_SIZE - 1)) & ~(FLASH_SECTOR_SIZE - 1)) / sizeof(uint32_t))
#define PS2_CAPTURE_WINDOW_SIZE 400
#define PS2_DATA_PIN 20
#define PS2_CLOCK_PIN 21
#define PS2_DATA_CLONE_PIN 0
#define PS2_CLOCK_CLONE_PIN 1
#define MAX_PARSED_BYTES 0xFFFFFFFF

typedef void (*menu_option_func_t)(void);

typedef struct
{
    const char *option_description;
    menu_option_func_t option_function;
} menu_definition_t;

volatile static uint32_t ps2_capture[MAX_CAPTURE_COUNT] = {0};
volatile static bool end_task_flag = false;
volatile static bool enable_flash_storage = true;
volatile static unsigned int ps2_pre_capture_count = 200;
volatile static uint32_t ps2_record_replay_iterations = 0;
__attribute__((section(".uninitialized_data"))) uint32_t overclock_flag;
extern char __flash_binary_end;

uint8_t __in_flash() flash_ps2_captures[FLASH_SECTOR_SIZE * FLASH_CAPTURE_SECTOR_COUNT]
    __attribute__((aligned(FLASH_SECTOR_SIZE))) = {
        [0 ...(FLASH_SECTOR_SIZE * FLASH_CAPTURE_SECTOR_COUNT) - 1] = 0xFF
    };

uint8_t __in_flash() flash_ps2_captures_info[FLASH_SECTOR_SIZE] __attribute__((aligned(FLASH_SECTOR_SIZE))) = {
    [0 ... FLASH_SECTOR_SIZE - 1] = 0xFF
};

static unsigned char *get_base_flash_space_addr(void)
{
    return (unsigned char *)XIP_BASE;
}

static uint32_t get_addr_start_captured_stored(void)
{
    return ((uint32_t)XIP_BASE) + ((uint32_t)flash_ps2_captures);
}

static uint32_t get_addr_end_captured_stored(void)
{
    return ((uint32_t)XIP_BASE) + sizeof(flash_ps2_captures);
}

static uint32_t get_size_captured_stored(void)
{
    return sizeof(flash_ps2_captures);
}

static void replay_ps2_capture(void)
{
    for (int i = 0; i < MAX_CAPTURE_COUNT; i++)
    {
        gpio_put_masked((1u << PS2_DATA_CLONE_PIN) | (1u << PS2_CLOCK_CLONE_PIN),
                        ((ps2_capture[i] >> PS2_DATA_PIN) & 0x3) << PS2_DATA_CLONE_PIN);
        sleep_us(2);
    }
}

static void capture_ps2_signals(unsigned int pre_capture_count)
{
    do
    {
        for (int i = 0; i < pre_capture_count; i++)
        {
            ps2_capture[i] = gpio_get_all();
            sleep_us(2);
        }
    } while (gpio_get(PS2_CLOCK_PIN));
    for (int i = pre_capture_count; i < MAX_CAPTURE_COUNT; i++)
    {
        ps2_capture[i] = gpio_get_all();
        sleep_us(2);
    }
}

static uint32_t get_flash_offset_from_addr(void *addr)
{
    return (uint32_t)addr - (uint32_t)XIP_BASE;
}

static void write_new_tracking(uint8_t *new_page_entry)
{
    flash_range_erase(get_flash_offset_from_addr((void *)flash_ps2_captures_info), FLASH_SECTOR_SIZE);
    flash_range_program(get_flash_offset_from_addr((void *)flash_ps2_captures_info), new_page_entry, FLASH_PAGE_SIZE);
}

static void *get_nth_capture_from_flash(uint32_t n)
{
    return flash_ps2_captures + (sizeof(ps2_capture) * n);
}

static uint32_t get_last_index_captures_stored_on_flash(void)
{
    if (*((uint32_t *)flash_ps2_captures_info) == 0xFFFFFFFF)
    {
        return (uint32_t)-1;
    }
    return *((uint32_t *)flash_ps2_captures_info);
}

static bool append_ps2_capture_to_flash(void)
{
    uint8_t new_page_entry[FLASH_PAGE_SIZE] = {0};
    *((uint32_t *)new_page_entry) = get_last_index_captures_stored_on_flash() + 1;
    if (*((uint32_t *)new_page_entry) >= get_size_captured_stored() / sizeof(ps2_capture))
    {
        printf("Max number of captures reached: %d\r\n", *((uint32_t *)new_page_entry));
        return false;
    }
    printf("Storing capture %d to flash...\r\n", *((uint32_t *)new_page_entry));
    uint32_t interrupts = save_and_disable_interrupts();
    write_new_tracking(new_page_entry);
    flash_range_program(
        get_flash_offset_from_addr(flash_ps2_captures + (sizeof(ps2_capture) * (*(uint32_t *)new_page_entry))),
        (const uint8_t *)ps2_capture, sizeof(ps2_capture));
    restore_interrupts(interrupts);
    return true;
}

static void delete_all_ps2_captures(void)
{
    uint8_t new_page_entry[FLASH_PAGE_SIZE] = {0};
    memset(new_page_entry, 0xFF, sizeof(new_page_entry));
    printf("Deleting all captures... wait and be patient!\r\n");
    fflush(stdout);
    sleep_ms(100);
    uint32_t interrupts = save_and_disable_interrupts();
    flash_range_erase(get_flash_offset_from_addr(flash_ps2_captures_info), sizeof(flash_ps2_captures_info));
    flash_range_erase(get_flash_offset_from_addr(flash_ps2_captures), sizeof(flash_ps2_captures));
    restore_interrupts(interrupts);
    printf("Done!\r\n");
}

static void erase_entire_flash_storage(void)
{
    flash_range_erase(0, PICO_FLASH_SIZE_BYTES);
    reset_usb_boot(0, 0);
}

static void free_all_pio_state_machines(PIO pio)
{
    for (int sm = 0; sm < 4; sm++)
    {
        if (pio_sm_is_claimed(pio, sm))
        {
            pio_sm_unclaim(pio, sm);
        }
    }
}

static void pio_destroy(void)
{
    free_all_pio_state_machines(pio0);
    free_all_pio_state_machines(pio1);
    pio_clear_instruction_memory(pio0);
    pio_clear_instruction_memory(pio1);
}

void core1_detect_glitches_main()
{
    sleep_ms(1000);
    puts("Core 1 started");
    while (gpio_get(PS2_CLOCK_PIN))
    {
        if (end_task_flag)
        {
            goto end;
        }
    }
    while (1)
    {
        if (gpio_get(PS2_CLOCK_PIN))
        {
            uint32_t start = time_us_32();
            while (gpio_get(PS2_CLOCK_PIN))
            {
                if (end_task_flag)
                {
                    goto end;
                }
            }
            uint32_t end = time_us_32();
            if (end - start < 20)
            {
                printf("[CORE1] glitch positive pulse detected: %d us (or less)\r\n", end - start);
            }
            while (!gpio_get(PS2_CLOCK_PIN))
            {
                if (end_task_flag)
                {
                    goto end;
                }
            }
        }
    }
end:
    while (1)
    {
        tight_loop_contents();
    }
}

static void init_gpios_for_out_replay(void)
{
    gpio_init(PS2_DATA_CLONE_PIN);
    gpio_set_dir(PS2_DATA_CLONE_PIN, GPIO_OUT);
    gpio_put(PS2_DATA_CLONE_PIN, false);
    gpio_init(PS2_CLOCK_CLONE_PIN);
    gpio_set_dir(PS2_CLOCK_CLONE_PIN, GPIO_OUT);
    gpio_put(PS2_CLOCK_CLONE_PIN, false);
}

static void init_gpios_for_capture(void)
{
    gpio_init(PS2_DATA_PIN);
    gpio_set_dir(PS2_DATA_PIN, GPIO_IN);
    gpio_pull_up(PS2_DATA_PIN);
    gpio_init(PS2_CLOCK_PIN);
    gpio_set_dir(PS2_CLOCK_PIN, GPIO_IN);
    gpio_pull_up(PS2_CLOCK_PIN);
    init_gpios_for_out_replay();
}

static void print_hexdump(uint8_t *data, uint32_t size)
{
    for (uint32_t i = 0; i < size; i++)
    {
        printf("%02X ", data[i]);
        if ((i + 1) % 16 == 0)
        {
            printf("\r\n");
        }
        fflush(stdout);
    }
    printf("\r\n");
}

static void print_buffer_as_array(unsigned int n, uint8_t *data, uint32_t size)
{
    printf("unsigned char array_%d[%d] = { ", n, size);
    fflush(stdout);
    for (uint32_t i = 0; i < size; i++)
    {
        printf("0x%02X", data[i]);
        if (i < size - 1)
        {
            printf(", ");
        }
        fflush(stdout);
    }
    printf(" };\r\n");
}

static void print_all_captures_as_hex_arrays(void)
{
    uint32_t total_captures = get_last_index_captures_stored_on_flash() + 1;
    printf("Total captures stored in flash: %d\r\n", total_captures);
    for (uint32_t i = 0; i < total_captures; i++)
    {
        printf("capture %d:\r\n", i);
        memset((void *)ps2_capture, 0, sizeof(ps2_capture));
        memcpy((void *)ps2_capture, get_nth_capture_from_flash(i), sizeof(ps2_capture));
        fflush(stdout);
        print_buffer_as_array(i, (uint8_t *)ps2_capture, sizeof(ps2_capture));
    }
}

typedef enum
{
    ST_IDLE = 0,
    ST_GOT_0,
    ST_GOT_0X_HEX1,
    ST_GOT_0X_HEX2
} parse_state_t;

static parse_state_t state = ST_IDLE;
static unsigned int wp = 0;
static unsigned char temp_byte = 0;
static unsigned char nibble_count = 0;

static unsigned char hex_value(char c)
{
    if (c >= '0' && c <= '9')
    {
        return (unsigned char)(c - '0');
    }
    else if (c >= 'a' && c <= 'f')
    {
        return (unsigned char)(c - 'a' + 10);
    }
    else if (c >= 'A' && c <= 'F')
    {
        return (unsigned char)(c - 'A' + 10);
    }
    return 0xFF;
}

static void parse_hex_character(char c)
{
    switch (state)
    {
    case ST_IDLE:
        if (c == '0')
        {
            state = ST_GOT_0;
        }
        break;
    case ST_GOT_0:
        if (c == 'x' || c == 'X')
        {
            state = ST_GOT_0X_HEX1;
            temp_byte = 0;
            nibble_count = 0;
        }
        else if (c == '0')
        {
        }
        else
        {
            state = ST_IDLE;
        }
        break;
    case ST_GOT_0X_HEX1: {
        unsigned char val = hex_value(c);
        if (val != 0xFF)
        {
            temp_byte = (val & 0x0F) << 4;
            state = ST_GOT_0X_HEX2;
        }
        else
        {
            state = ST_IDLE;
        }
    }
    break;
    case ST_GOT_0X_HEX2: {
        unsigned char val = hex_value(c);
        if (val != 0xFF)
        {
            temp_byte |= (val & 0x0F);
            if (wp < MAX_PARSED_BYTES)
            {
                ((uint8_t *)ps2_capture)[wp++] = temp_byte;
            }
            state = ST_IDLE;
        }
        else
        {
            state = ST_IDLE;
        }
    }
    break;
    }
}

void core1_import_ps2_captures_main()
{
    sleep_ms(1000);
    puts("Core 1 started");
    int c;
    delete_all_ps2_captures();
    fflush(stdin);
    printf("\r\nInsert (Max %d) captures, (this is slow...). Example:\r\n"
           "unsigned char array_9[] = { 0x03, 0xEC, 0x33 };\r\n",
           get_size_captured_stored() / sizeof(ps2_capture));
    for (int i = 0, j = 0; i < (int)(get_size_captured_stored() / sizeof(ps2_capture)); i++)
    {
        state = ST_IDLE;
        wp = 0;
        temp_byte = 0;
        nibble_count = 0;
        printf("\r\nInsert capture [%d] (press '!' + ENTER to exit):\r\n", i);
        j = 0;
        memset((void *)ps2_capture, 0, sizeof(ps2_capture));
        while ((c = getchar()) != ';')
        {
            if (c == '!')
            {
                goto end;
            }
            if (j++ % 100001 == 0 || j == 0)
            {
                printf("..%c", c);
            }
            parse_hex_character((char)c);
        }
        printf("\r\n\r\nTotal imported: %d bytes\r\n", wp);
        if (append_ps2_capture_to_flash())
        {
            printf("\r\ncaptured saved to flash!\r\n");
        }
        else
        {
            printf("\r\ncaptured not saved!\r\n");
        }
    }
    fflush(stdin);
end:
    end_task_flag = true;
    while (1)
    {
        tight_loop_contents();
    }
}

static void import_ps2_captures(void)
{
    end_task_flag = false;
    fflush(stdout);
    fflush(stdin);
    stdio_set_chars_available_callback(NULL, NULL);
    multicore_launch_core1(core1_import_ps2_captures_main);
    sleep_ms(1);
    multicore_reset_core1();
    multicore_launch_core1(core1_import_ps2_captures_main);
    while (!end_task_flag)
    {
        tight_loop_contents();
    }
}

void core1_record_and_replay_main()
{
    sleep_ms(1000);
    puts("Core 1 started");
    init_gpios_for_capture();
    sleep_ms(100);
    printf("MAX_CAPTURE_COUNT: %d (bytes: %d)\r\n", (int)(sizeof(ps2_capture) / sizeof(*ps2_capture)),
           (int)sizeof(ps2_capture));
    printf("waiting for PS2_CLOCK_PIN %d to go low...\r\n", PS2_CLOCK_PIN);
    memset((void *)ps2_capture, 0, sizeof(ps2_capture));
    while (ps2_record_replay_iterations == 0 || ps2_record_replay_iterations-- > 0)
    {
        capture_ps2_signals(ps2_pre_capture_count);
        if (enable_flash_storage)
        {
            if (append_ps2_capture_to_flash())
            {
                printf("\r\ncaptured saved!\r\n");
            }
            else
            {
                printf("\r\ncaptured not saved!, Press any key to return\r\n");
            }
        }
        replay_ps2_capture();
    }
    end_task_flag = true;
    printf("\r\nDone! press any key to continue\r\n");
    while (1)
    {
        tight_loop_contents();
    }
}

static void stdin_detected_callback(void *arg)
{
    end_task_flag = true;
}

static void detect_ps2_glitches(void)
{
    gpio_init(PS2_CLOCK_PIN);
    gpio_set_dir(PS2_CLOCK_PIN, GPIO_IN);
    gpio_pull_up(PS2_CLOCK_PIN);
    sleep_ms(100);
    puts("\r\npress any key to stop glitch detector\r\n");
    fflush(stdout);
    fflush(stdin);
    stdio_set_chars_available_callback(NULL, NULL);
    end_task_flag = false;
    stdio_set_chars_available_callback(stdin_detected_callback, NULL);
    printf("[CORE0] Detecting glitch (<20us) pulses on PS2_CLOCK_PIN...\r\n");
    multicore_launch_core1(core1_detect_glitches_main);
    sleep_ms(1);
    multicore_reset_core1();
    multicore_launch_core1(core1_detect_glitches_main);
    while (!gpio_get(PS2_CLOCK_PIN))
    {
        if (end_task_flag)
        {
            return;
        }
    }
    while (1)
    {
        if (!gpio_get(PS2_CLOCK_PIN))
        {
            uint32_t start = time_us_32();
            while (!gpio_get(PS2_CLOCK_PIN))
            {
                if (end_task_flag)
                {
                    return;
                }
            }
            uint32_t end = time_us_32();
            if (end - start < 20)
            {
                printf("[CORE0] glitch negative pulse detected: %d us (or less)\r\n", end - start);
            }
            while (gpio_get(PS2_CLOCK_PIN))
            {
                if (end_task_flag)
                {
                    return;
                }
            }
        }
    }
    while (1)
    {
        if (end_task_flag)
        {
            return;
        }
    }
}

static void record_and_replay_ps2_signals(void)
{
    end_task_flag = false;
    fflush(stdout);
    fflush(stdin);
    stdio_set_chars_available_callback(NULL, NULL);
    multicore_launch_core1(core1_record_and_replay_main);
    sleep_ms(1);
    multicore_reset_core1();
    multicore_launch_core1(core1_record_and_replay_main);
    puts("\r\npress any key to stop record and play\r\n");
    fflush(stdout);
    fflush(stdin);
    getchar();
    end_task_flag = true;
}

void core1_replay_stored_main()
{
    sleep_ms(1000);
    puts("Core 1 started");
    init_gpios_for_out_replay();
    sleep_ms(100);
    uint32_t total_captures = get_last_index_captures_stored_on_flash() + 1;
    printf("\r\nTotal captures stored in flash: %d\r\n", total_captures);
    init_gpios_for_capture();
    if (total_captures > 0)
    {
        printf("Playing %d captures stored in flash\r\n\r\n", total_captures);
        for (uint32_t i = 0; i < total_captures; i++)
        {
            printf("Playing %d\r\n", i);
            memcpy((void *)ps2_capture, get_nth_capture_from_flash(i), sizeof(ps2_capture));
            if (ps2_capture != NULL)
            {
                replay_ps2_capture();
            }
        }
    }
    else
    {
        printf("No captures stored in flash\r\n");
    }
    end_task_flag = true;
    puts("\r\nDone! press any key to continue\r\n");
    while (1)
    {
        tight_loop_contents();
    }
}

static void replay_all_stored_captures(void)
{
    end_task_flag = false;
    fflush(stdout);
    fflush(stdin);
    stdio_set_chars_available_callback(NULL, NULL);
    multicore_launch_core1(core1_replay_stored_main);
    sleep_ms(1);
    multicore_reset_core1();
    multicore_launch_core1(core1_replay_stored_main);
    puts("\r\npress any key to stop play stored\r\n");
    fflush(stdout);
    fflush(stdin);
    getchar();
    end_task_flag = true;
}

static void print_flash_capture_summary(void)
{
    printf("Total captures stored in flash: %d\r\n", get_last_index_captures_stored_on_flash() + 1);
    printf("-\r\n");
}

static void replay_single_stored_capture(void)
{
    init_gpios_for_out_replay();
    sleep_ms(100);
    unsigned int capture_number = 0;
    do
    {
        printf("Press ENTER to return to main menu\r\n");
        printf("Enter the capture index to play (%d-%d): ", 0, get_last_index_captures_stored_on_flash());
        capture_number = getchar() - '0';
        printf("%d\r\n", capture_number);
        if (capture_number >= get_last_index_captures_stored_on_flash() + 1)
        {
            capture_number = 0;
            printf("Invalid capture number\r\n");
            return;
        }
        printf("Playing capture %d\r\n", capture_number);
        memcpy((void *)ps2_capture, get_nth_capture_from_flash(capture_number), sizeof(ps2_capture));
        replay_ps2_capture();
    } while (capture_number);
}

static bool ask_to_continue_dangerous_option(void)
{
    printf("This option can delete flash content, Are you sure? [y/n]: (n)");
    char c = getchar();
    printf("%c\r\n", c);
    return (c == 'y' || c == 'Y');
}

static void menu_action_glitch_detector(void)
{
    detect_ps2_glitches();
    multicore_reset_core1();
    stdio_set_chars_available_callback(NULL, NULL);
}

static void menu_action_record_and_replay(void)
{
    record_and_replay_ps2_signals();
    multicore_reset_core1();
    stdio_set_chars_available_callback(NULL, NULL);
}

static void menu_action_delete_all_captures(void)
{
    if (ask_to_continue_dangerous_option())
    {
        delete_all_ps2_captures();
    }
}

static void menu_action_replay_all_stored(void)
{
    replay_all_stored_captures();
    multicore_reset_core1();
    stdio_set_chars_available_callback(NULL, NULL);
}

static void menu_action_export_captures(void)
{
    print_all_captures_as_hex_arrays();
    multicore_reset_core1();
    stdio_set_chars_available_callback(NULL, NULL);
}

static void menu_action_replay_single_capture(void)
{
    replay_single_stored_capture();
}

static void menu_action_import_captures(void)
{
    if (ask_to_continue_dangerous_option())
    {
        import_ps2_captures();
        multicore_reset_core1();
        stdio_set_chars_available_callback(NULL, NULL);
    }
}

static void menu_action_erase_flash(void)
{
    if (ask_to_continue_dangerous_option())
    {
        erase_entire_flash_storage();
    }
}

static void menu_action_toggle_overclock(void)
{
    printf("Please wait... and re-connect\r\n");
    fflush(stdout);
    overclock_flag = (overclock_flag == 0x69696969) ? 0 : 0x69696969;
    sleep_ms(100);
    watchdog_reboot(0, 0, 0);
    while (1)
    {
        tight_loop_contents();
    }
}

static void menu_action_free(void)
{
    printf("Free option selected\r\n");
}

static menu_definition_t menu_definitions[] = {{"Glitch detector", menu_action_glitch_detector},
                                               {"Record, replay and store", menu_action_record_and_replay},
                                               {"Delete all captures stored in flash", menu_action_delete_all_captures},
                                               {"Play all captures stored in flash", menu_action_replay_all_stored},
                                               {"Export all captures stored in flash", menu_action_export_captures},
                                               {"Play one capture stored in flash", menu_action_replay_single_capture},
                                               {"Import all captures to flash", menu_action_import_captures},
                                               {"Nuke PICO FLASH (erase full flash)", menu_action_erase_flash},
                                               {"Enable/Disable Overclock 250mhz", menu_action_toggle_overclock},
                                               {"FREE", menu_action_free}};

int main(void)
{
    set_sys_clock_khz((overclock_flag == 0x69696969) ? 250000 : 125000, true);
    sleep_ms(3000);
    tud_disconnect();
    stdio_init_all();
    sleep_ms(3000);
    tud_connect();
    pio_destroy();
    printf("Flash end address: 0x%08X\r\n", (unsigned int)&__flash_binary_end);
    printf("Flash size: %d bytes\r\n", PICO_FLASH_SIZE_BYTES);
    printf("flash_ps2_captures flash addr: 0x%08X\r\n", (unsigned int)flash_ps2_captures);
    printf("flash_ps2_captures size: %d bytes\r\n", (int)get_size_captured_stored());
    printf("Max number of captures: %d\r\n", (int)(get_size_captured_stored() / sizeof(ps2_capture)));
    unsigned char option = 0;
    do
    {
        gpio_init(PS2_DATA_PIN);
        gpio_init(PS2_CLOCK_PIN);
        gpio_init(PS2_DATA_CLONE_PIN);
        gpio_init(PS2_CLOCK_CLONE_PIN);
        printf("\r\n\r\npico_ps2_diagnostic_tool started! v%d Build Date %s %s\r\n"
               "https://github.com/therealdreg/pico_ps2_diagnostic_tool\r\n"
               "MIT License David Reguera Garcia aka Dreg\r\n"
               "X @therealdreg dreg@rootkit.es\r\n"
               "---------------------------------------------------------------\r\n",
               FVER, __DATE__, __TIME__);
        printf("overclocked 250mhz: %s\r\n", (overclock_flag == 0x69696969) ? "true" : "false");
        print_flash_capture_summary();
        printf("Options:\r\n");
        for (int i = 0; i < (int)(sizeof(menu_definitions) / sizeof(*menu_definitions)); i++)
        {
            printf("%d: %s\r\n", i, menu_definitions[i].option_description);
        }
        printf("Select an option: ");
        option = getchar();
        printf("%c\r\n", option);
        if (option >= '0' && option <= '9')
        {
            printf("You selected option %c: %s\r\n", option, menu_definitions[option - '0'].option_description);
            menu_definitions[option - '0'].option_function();
        }
        else
        {
            printf("Invalid option\r\n");
        }
    } while (1);
    return 0;
}

/*
volatile static int capture_channels_sm;
volatile static int play_channels_sm;

    while (true)
    {
        if (!pio_sm_is_rx_fifo_empty(pio0, capture_channels_sm))
        {
            captures[circular_index] = pio_sm_get_blocking(pio0, capture_channels_sm);
            circular_index = (circular_index + 1) % number_of_captures;

            // Detectar disparo
            if (!triggered && !gpio_get(21))
            {
                triggered = true;
                trigger_index = circular_index;
                post_trigger_captured = 0;
            }
            else if (triggered)
            {
                // Contar muestras luego del trigger
                post_trigger_captured++;
                if (post_trigger_captured >= (number_of_captures - window_size))
                {
                    break;
                }
            }
        }
    }

    {
        uint32_t temp[number_of_captures];
        unsigned int start_pre = (trigger_index + number_of_captures - window_size) % number_of_captures;
        unsigned int idx = 0;

        for (unsigned int i = 0; i < window_size; i++)
        {
            temp[idx++] = captures[(start_pre + i) % number_of_captures];
        }

        for (unsigned int i = 0; i < (number_of_captures - window_size); i++)
        {
            temp[idx++] = captures[(trigger_index + i) % number_of_captures];
        }

        for (unsigned int i = 0; i < number_of_captures; i++)
        {
            captures[i] = temp[i];
        }
    }

    printf("Capture complete.\r\n");

    printf("Playing %d captures in loop\r\n", number_of_captures);
    while (1)
    {
        for (int j = 0; j < number_of_captures; j++)
        {
            pio_sm_put_blocking(pio0, play_channels_sm, captures[j]);
        }
        sleep_ms(3000);
        break;
    }

    while (1)
    {
        tight_loop_contents();
    }

void OLDcore1_main()
{
    puts("Core 1 started");
    pio_destroy();

    gpio_init(CLOCK_GPIO);
    gpio_set_dir(CLOCK_GPIO, GPIO_IN);
    gpio_pull_up(CLOCK_GPIO);

    sleep_ms(1000);

    while (gpio_get(CLOCK_GPIO))
        ;
    while (1)
    {
        if (gpio_get(CLOCK_GPIO))
        {
            uint32_t start = time_us_32();
            while (gpio_get(CLOCK_GPIO))
            {
                tight_loop_contents();
            }
            uint32_t end = time_us_32();
            if (end - start < 20)
            {
                printf("[CORE1] glitch positive pulse detected: %d us (or less)\r\n", end - start);
            }
            while (!gpio_get(CLOCK_GPIO))
            {
                tight_loop_contents();
            }
        }
    }
}

    unsigned int trigger_pin = 0;
    unsigned int trigger_pin_play = 1;
    unsigned int first_gpio = 20;
    unsigned int first_output_pin = 8;
    unsigned int nr_channels = 2;
    float speed_hz = 125000000.0; // 125000000 hz == 125mhz
    bool pull_ups = true;

    // init trigger pin
    gpio_init(trigger_pin);
    gpio_set_dir(trigger_pin, GPIO_IN);
    gpio_pull_up(trigger_pin);

    gpio_init(trigger_pin_play);
    gpio_set_dir(trigger_pin_play, GPIO_OUT);
    gpio_put(trigger_pin_play, true);

    pio_destroy();

    // init GPIOS as input
    for (unsigned int i = 0; i < nr_channels; i++)
    {
        gpio_init(first_gpio + i);
        gpio_set_dir(first_gpio + i, GPIO_IN);
        if (pull_ups)
        {
            gpio_pull_up(first_gpio + i);
        }
    }

    for (size_t i = 0; i < nr_channels; i++)
    {
        gpio_init(first_output_pin + i);
        gpio_set_dir(first_output_pin + i, GPIO_OUT);
        gpio_put(first_output_pin + i, false);
        pio_gpio_init(pio0, first_output_pin + i);
    }

    float clkdiv = 6.0f;

    capture_channels_sm = pio_claim_unused_sm(pio0, true);
    uint offset_capture_channels = pio_add_program(pio0, &capture_channels_program);
    pio_sm_set_consecutive_pindirs(pio0, capture_channels_sm, first_gpio, nr_channels, false);
    pio_sm_config c_capture_channels = capture_channels_program_get_default_config(offset_capture_channels);
    sm_config_set_in_pins(&c_capture_channels, first_gpio);
    sm_config_set_in_shift(&c_capture_channels, false, true, nr_channels);
    sm_config_set_jmp_pin(&c_capture_channels, trigger_pin);
    sm_config_set_fifo_join(&c_capture_channels, PIO_FIFO_JOIN_RX);
    sm_config_set_clkdiv(&c_capture_channels, clkdiv);
    pio_sm_init(pio0, capture_channels_sm, offset_capture_channels, &c_capture_channels);
    pio_sm_set_enabled(pio0, capture_channels_sm, false);
    pio_sm_clear_fifos(pio0, capture_channels_sm);
    pio_sm_restart(pio0, capture_channels_sm);
    pio_sm_clkdiv_restart(pio0, capture_channels_sm);
    pio_sm_set_enabled(pio0, capture_channels_sm, true);
    pio_sm_exec(pio0, capture_channels_sm, pio_encode_jmp(offset_capture_channels));

    play_channels_sm = pio_claim_unused_sm(pio0, true);
    uint offset_play_channels = pio_add_program(pio0, &play_channels_program);
    pio_sm_set_consecutive_pindirs(pio0, play_channels_sm, first_output_pin, nr_channels, true);
    pio_sm_config c_play_channels = play_channels_program_get_default_config(offset_play_channels);
    sm_config_set_out_pins(&c_play_channels, first_output_pin, nr_channels);
    sm_config_set_out_shift(&c_play_channels, true, true, nr_channels);
    sm_config_set_jmp_pin(&c_play_channels, trigger_pin_play);
    sm_config_set_fifo_join(&c_play_channels, PIO_FIFO_JOIN_TX);
    sm_config_set_clkdiv(&c_play_channels, clkdiv);
    pio_sm_init(pio0, play_channels_sm, offset_play_channels, &c_play_channels);
    pio_sm_set_enabled(pio0, play_channels_sm, false);
    pio_sm_clear_fifos(pio0, play_channels_sm);
    pio_sm_restart(pio0, play_channels_sm);
    pio_sm_clkdiv_restart(pio0, play_channels_sm);
    pio_sm_set_enabled(pio0, play_channels_sm, true);
    pio_sm_exec(pio0, play_channels_sm, pio_encode_jmp(offset_play_channels));

    gpio_put(trigger_pin_play, false);












#define CLOCK_GPIO 21
#define DAT_GPIO 20
#define SIDESET_BASE_GPIO 18

    gpio_init(CLOCK_GPIO);
    gpio_set_dir(CLOCK_GPIO, GPIO_IN);
    gpio_pull_up(CLOCK_GPIO);

    gpio_init(DAT_GPIO);
    gpio_set_dir(DAT_GPIO, GPIO_IN);
    gpio_pull_up(DAT_GPIO);

    gpio_init(CLOCK_GPIO + 1);
    gpio_set_dir(CLOCK_GPIO + 1, GPIO_OUT);
    gpio_put(CLOCK_GPIO + 1, false);
    pio_gpio_init(pio0, CLOCK_GPIO + 1);

    gpio_init(SIDESET_BASE_GPIO);
    gpio_set_dir(SIDESET_BASE_GPIO, GPIO_OUT);
    gpio_put(SIDESET_BASE_GPIO, false);
    pio_gpio_init(pio0, SIDESET_BASE_GPIO);

    gpio_init(SIDESET_BASE_GPIO + 1);
    gpio_set_dir(SIDESET_BASE_GPIO + 1, GPIO_OUT);
    gpio_put(SIDESET_BASE_GPIO + 1, false);
    pio_gpio_init(pio0, SIDESET_BASE_GPIO + 1);

    int glitch_fast_rise_sm = pio_claim_unused_sm(pio0, true);
    uint offset_glitch_fast_rise = pio_add_program(pio0, &glitch_fast_rise_program);
    pio_sm_set_consecutive_pindirs(pio0, glitch_fast_rise_sm, CLOCK_GPIO, 2, false);
    pio_sm_set_consecutive_pindirs(pio0, glitch_fast_rise_sm, SIDESET_BASE_GPIO, 2, true);
    pio_sm_config c_glitch_fast_rise = glitch_fast_rise_program_get_default_config(offset_glitch_fast_rise);
    sm_config_set_in_pins(&c_glitch_fast_rise, CLOCK_GPIO);
    sm_config_set_in_shift(&c_glitch_fast_rise, false, true, 32);
    sm_config_set_jmp_pin(&c_glitch_fast_rise, CLOCK_GPIO);
    sm_config_set_sideset_pins(&c_glitch_fast_rise, SIDESET_BASE_GPIO);
    sm_config_set_fifo_join(&c_glitch_fast_rise, PIO_FIFO_JOIN_RX);
    // 4 MHZ
    sm_config_set_clkdiv(&c_glitch_fast_rise, (float)clock_get_hz(clk_sys) / 4000000.0f);
    pio_sm_init(pio0, glitch_fast_rise_sm, offset_glitch_fast_rise, &c_glitch_fast_rise);

    pio_sm_set_enabled(pio0, glitch_fast_rise_sm, false);
    pio_sm_clear_fifos(pio0, glitch_fast_rise_sm);
    pio_sm_restart(pio0, glitch_fast_rise_sm);
    pio_sm_clkdiv_restart(pio0, glitch_fast_rise_sm);

pio_sm_set_enabled(pio0, glitch_fast_rise_sm, true);

while (1)
{
    tight_loop_contents();
}

int fast_rise_counter_sm = pio_claim_unused_sm(pio0, true);
uint offset_fast_rise_counter = pio_add_program(pio0, &fast_rise_counter_program);
pio_sm_set_consecutive_pindirs(pio0, fast_rise_counter_sm, SIDESET_BASE_GPIO, 4, false);
pio_sm_set_consecutive_pindirs(pio0, fast_rise_counter_sm, CLOCK_GPIO + 1, 1, true);
pio_sm_config c_fast_rise_counter = fast_rise_counter_program_get_default_config(offset_fast_rise_counter);
sm_config_set_in_pins(&c_fast_rise_counter, SIDESET_BASE_GPIO);
sm_config_set_in_shift(&c_fast_rise_counter, false, true, 32);
sm_config_set_jmp_pin(&c_fast_rise_counter, CLOCK_GPIO);
sm_config_set_sideset_pins(&c_fast_rise_counter, CLOCK_GPIO + 1);
sm_config_set_out_pins(&c_fast_rise_counter, CLOCK_GPIO + 1, 1);
sm_config_set_fifo_join(&c_fast_rise_counter, PIO_FIFO_JOIN_RX);
// 250mhz
sm_config_set_clkdiv(&c_fast_rise_counter, 1.0f);
pio_sm_init(pio0, fast_rise_counter_sm, offset_fast_rise_counter, &c_fast_rise_counter);
pio_sm_set_enabled(pio0, fast_rise_counter_sm, false);
pio_sm_clear_fifos(pio0, fast_rise_counter_sm);
pio_sm_restart(pio0, fast_rise_counter_sm);
pio_sm_clkdiv_restart(pio0, fast_rise_counter_sm);
pio_sm_set_enabled(pio0, fast_rise_counter_sm, true);

*/
