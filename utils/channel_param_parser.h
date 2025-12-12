#ifndef CHANNEL_PARSER_H
#define CHANNEL_PARSER_H

// Global status variables (external linkage for main.c)
extern int vin_isp_is_online;    // VIN ISP online status (default: 0)
extern int isp_is_online_vse;    // ISP VSE online status (default: 0)

/**
 * @brief Parse channel string (split by ':' and validate each sub-string)
 * @param input Input string (e.g., "vo:io", "vf:if:vo")
 * @return 0 on success, -1 on failure
 */
int parse_channel_string(const char *input);

#endif // CHANNEL_PARSER_H