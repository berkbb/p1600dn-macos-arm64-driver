#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define main p1600dn_status_cli_main
#include "../src/p1600dn_status.c"
#undef main

typedef struct {
    unsigned char *data;
    size_t len;
} CapturedOutput;

static int contains(const CapturedOutput *output, const char *text) {
    return memmem(output->data, output->len, text, strlen(text)) != NULL;
}

static CapturedOutput capture_response(const char *label, const char *uri,
                                       ipp_t *response, int *has_supplies) {
    FILE *capture = tmpfile();
    assert(capture != NULL);
    int saved_stdout = dup(STDOUT_FILENO);
    assert(saved_stdout >= 0);
    assert(fflush(stdout) == 0);
    assert(dup2(fileno(capture), STDOUT_FILENO) >= 0);
    *has_supplies = print_response(label, uri, response);
    assert(fflush(stdout) == 0);
    assert(dup2(saved_stdout, STDOUT_FILENO) >= 0);
    close(saved_stdout);

    assert(fseek(capture, 0, SEEK_END) == 0);
    long size = ftell(capture);
    assert(size >= 0);
    assert(fseek(capture, 0, SEEK_SET) == 0);
    CapturedOutput output;
    output.len = (size_t)size;
    output.data = malloc(output.len == 0 ? 1 : output.len);
    assert(output.data != NULL);
    assert(fread(output.data, 1, output.len, capture) == output.len);
    assert(fclose(capture) == 0);
    return output;
}

static void free_output(CapturedOutput *output) {
    free(output->data);
    output->data = NULL;
    output->len = 0;
}

static void test_connection_and_state_classification(void) {
    assert(strcmp(connection_type(NULL), "Bilinmiyor") == 0);
    assert(strcmp(connection_type("usb://HP/P1606dn"), "USB") == 0);
    assert(strcmp(connection_type("USB://HP/P1606dn"), "USB") == 0);
    assert(strcmp(connection_type("ipp://printer/ipp/print"), "Ağ (IPP)") == 0);
    assert(strcmp(connection_type("ipps://printer/ipp/print"), "Ağ (IPP)") == 0);
    assert(strcmp(connection_type("dnssd://printer"), "Ağ (Bonjour / Wi-Fi veya Ethernet)") == 0);
    assert(strcmp(connection_type("socket://printer:9100"), "Ağ (AppSocket / Ethernet veya Wi-Fi)") == 0);
    assert(strcmp(connection_type("lpd://printer/queue"), "CUPS arka ucu") == 0);

    assert(strcmp(state_name(IPP_PSTATE_IDLE), "Hazır") == 0);
    assert(strcmp(state_name(IPP_PSTATE_PROCESSING), "Yazdırıyor") == 0);
    assert(strcmp(state_name(IPP_PSTATE_STOPPED), "Durduruldu / dikkat gerekli") == 0);
    assert(strcmp(state_name(0), "Bilinmiyor") == 0);

    assert(is_ipp_uri("ipp://printer/ipp/print"));
    assert(is_ipp_uri("IPPS://printer/ipp/print"));
    assert(!is_ipp_uri(NULL));
    assert(!is_ipp_uri("http://printer"));
    assert(!is_ipp_uri("socket://printer:9100"));
    puts("    [PASS] Status Connection/State/URI Classification Matrix");
}

static ipp_t *make_complete_response(void) {
    ipp_t *response = ippNew();
    assert(response != NULL);
    ippAddInteger(response, IPP_TAG_PRINTER, IPP_TAG_ENUM,
                  "printer-state", IPP_PSTATE_IDLE);
    ippAddString(response, IPP_TAG_PRINTER, IPP_TAG_TEXT,
                 "printer-state-message", NULL, "Ready for exhaustive test");
    const char *reasons[] = {"none", "media-empty", "toner-low"};
    ippAddStrings(response, IPP_TAG_PRINTER, IPP_TAG_KEYWORD,
                  "printer-state-reasons", 3, NULL, reasons);

    const int levels[] = {100, 0, -1, -2, -3, 101};
    const char *names[] = {"Full", "Empty", "Unavailable", "Unknown", "Unsupported", "Invalid"};
    const char *types[] = {"toner", "toner", "toner", "toner", "toner", "toner"};
    ippAddIntegers(response, IPP_TAG_PRINTER, IPP_TAG_INTEGER,
                   "marker-levels", 6, levels);
    ippAddStrings(response, IPP_TAG_PRINTER, IPP_TAG_NAME,
                  "marker-names", 6, NULL, names);
    ippAddStrings(response, IPP_TAG_PRINTER, IPP_TAG_KEYWORD,
                  "marker-types", 6, NULL, types);
    ippAddString(response, IPP_TAG_PRINTER, IPP_TAG_TEXT,
                 "marker-message", NULL, "Synthetic marker message");
    return response;
}

static void test_complete_ipp_response(void) {
    ipp_t *response = make_complete_response();
    int has_supplies = 0;
    CapturedOutput output = capture_response("Synthetic P1606dn", "usb://HP/P1606dn",
                                             response, &has_supplies);
    assert(has_supplies == 1);
    assert(contains(&output, "Synthetic P1606dn"));
    assert(contains(&output, "Bağlantı: USB"));
    assert(contains(&output, "Durum: Hazır"));
    assert(contains(&output, "Ready for exhaustive test"));
    assert(contains(&output, "Durum nedeni: media-empty"));
    assert(contains(&output, "Durum nedeni: toner-low"));
    assert(!contains(&output, "Durum nedeni: none"));
    assert(contains(&output, "Full (toner): 100%"));
    assert(contains(&output, "Empty (toner): 0%"));
    assert(contains(&output, "Unavailable (toner): kullanılamıyor"));
    assert(contains(&output, "Unknown (toner): bilinmiyor"));
    assert(contains(&output, "Unsupported (toner): seviyesi desteklenmiyor"));
    assert(contains(&output, "Invalid (toner): bilinmiyor"));
    assert(contains(&output, "Synthetic marker message"));
    free_output(&output);
    ippDelete(response);
    puts("    [PASS] Complete Synthetic IPP State/Reason/Supply-Level Matrix");
}

static void test_missing_and_partial_supplies(void) {
    ipp_t *missing = ippNew();
    assert(missing != NULL);
    int has_supplies = 1;
    CapturedOutput missing_output = capture_response("Missing", NULL, missing, &has_supplies);
    assert(has_supplies == 0);
    assert(contains(&missing_output, "Bağlantı: Bilinmiyor"));
    assert(contains(&missing_output, "Durum: Bilinmiyor"));
    assert(contains(&missing_output, "Yazıcı bu bilgiyi paylaşmıyor"));
    free_output(&missing_output);
    ippDelete(missing);

    ipp_t *partial = ippNew();
    assert(partial != NULL);
    const int levels[] = {50, 25};
    const char *names[] = {"", "Second"};
    ippAddIntegers(partial, IPP_TAG_PRINTER, IPP_TAG_INTEGER,
                   "marker-levels", 2, levels);
    ippAddStrings(partial, IPP_TAG_PRINTER, IPP_TAG_NAME,
                  "marker-names", 2, NULL, names);
    CapturedOutput partial_output = capture_response("Partial", "lpd://printer", partial,
                                                     &has_supplies);
    assert(has_supplies == 1);
    assert(contains(&partial_output, "Sarf malzemesi: 50%"));
    assert(contains(&partial_output, "Second: 25%"));
    free_output(&partial_output);
    ippDelete(partial);
    puts("    [PASS] Missing/Partial IPP Supply Fallback Matrix");
}

static void test_rejected_remote_queue_and_cli_paths(void) {
    int saved_stderr = dup(STDERR_FILENO);
    int null_fd = open("/dev/null", O_WRONLY);
    assert(saved_stderr >= 0 && null_fd >= 0);
    assert(fflush(stderr) == 0);
    assert(dup2(null_fd, STDERR_FILENO) >= 0);
    close(null_fd);

    int invalid_scheme = query_remote_ipp("http://127.0.0.1/printer", "Invalid");
    int malformed_uri = query_remote_ipp("ipp://", "Malformed");
    int unavailable_endpoint = query_remote_ipp("ipp://127.0.0.1:1/ipp/print", "Unavailable");
    int missing_queue = query_queue("__p1600dn_definitely_missing_status_queue__");
    char *invalid_arity[] = {"status", "one", "two", "three", NULL};
    int invalid_arity_status = p1600dn_status_cli_main(4, invalid_arity);
    char *invalid_uri[] = {"status", "--uri", "socket://127.0.0.1:9100", NULL};
    int invalid_uri_status = p1600dn_status_cli_main(3, invalid_uri);

    assert(fflush(stderr) == 0);
    assert(dup2(saved_stderr, STDERR_FILENO) >= 0);
    close(saved_stderr);
    assert(invalid_scheme == 0);
    assert(malformed_uri == 0);
    assert(unavailable_endpoint == 0);
    assert(missing_queue == 0);
    assert(invalid_arity_status == 1);
    assert(invalid_uri_status == 1);
    puts("    [PASS] Status Invalid URI/Unavailable Endpoint/Missing Queue/CLI Paths");
}

static void test_english_localization(void) {
    status_set_language(STATUS_LANG_EN);
    assert(strcmp(connection_type(NULL), "Unknown") == 0);
    assert(strcmp(connection_type("usb://HP/P1606dn"), "USB") == 0);
    assert(strcmp(connection_type("ipp://printer/ipp/print"), "Network (IPP)") == 0);
    assert(strcmp(connection_type("dnssd://printer"), "Network (Bonjour / Wi-Fi or Ethernet)") == 0);
    assert(strcmp(connection_type("socket://printer:9100"), "Network (AppSocket / Ethernet or Wi-Fi)") == 0);
    assert(strcmp(connection_type("lpd://printer/queue"), "CUPS backend") == 0);

    assert(strcmp(state_name(IPP_PSTATE_IDLE), "Idle / Ready") == 0);
    assert(strcmp(state_name(IPP_PSTATE_PROCESSING), "Printing") == 0);
    assert(strcmp(state_name(IPP_PSTATE_STOPPED), "Stopped / Attention required") == 0);
    assert(strcmp(state_name(0), "Unknown") == 0);

    ipp_t *response = make_complete_response();
    int has_supplies = 0;
    CapturedOutput output = capture_response("Synthetic P1606dn", "usb://HP/P1606dn",
                                             response, &has_supplies);
    assert(has_supplies == 1);
    assert(contains(&output, "Synthetic P1606dn"));
    assert(contains(&output, "Connection: USB"));
    assert(contains(&output, "State: Idle / Ready"));
    assert(contains(&output, "Ready for exhaustive test"));
    assert(contains(&output, "State reason: media-empty"));
    assert(contains(&output, "State reason: toner-low"));
    assert(!contains(&output, "State reason: none"));
    assert(contains(&output, "Full (toner): 100%"));
    assert(contains(&output, "Empty (toner): 0%"));
    assert(contains(&output, "Unavailable (toner): unavailable"));
    assert(contains(&output, "Unknown (toner): unknown"));
    assert(contains(&output, "Unsupported (toner): level unsupported"));
    assert(contains(&output, "Invalid (toner): unknown"));
    assert(contains(&output, "Marker message: Synthetic marker message"));
    free_output(&output);
    ippDelete(response);

    /* Test missing supplies fallback in English */
    ipp_t *missing = ippNew();
    assert(missing != NULL);
    has_supplies = 1;
    CapturedOutput missing_output = capture_response("Missing", NULL, missing, &has_supplies);
    assert(has_supplies == 0);
    assert(contains(&missing_output, "Connection: Unknown"));
    assert(contains(&missing_output, "State: Unknown"));
    assert(contains(&missing_output, "Printer does not report this information"));
    free_output(&missing_output);
    ippDelete(missing);

    /* Reset to TR */
    status_set_language(STATUS_LANG_TR);
    assert(status_get_language() == STATUS_LANG_TR);
    puts("    [PASS] Status English and Turkish Localization Matrix");
}

int main(void) {
    puts("[*] Starting detailed status utility tests...");
    test_connection_and_state_classification();
    test_complete_ipp_response();
    test_missing_and_partial_supplies();
    test_rejected_remote_queue_and_cli_paths();
    test_english_localization();
    puts("[+] Detailed status utility tests passed.");
    return 0;
}
