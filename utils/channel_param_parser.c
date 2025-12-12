#include <stdio.h>
#include <string.h>

// Define global status variables
int vin_isp_is_online = 0;
int isp_vse_is_online = 0;

typedef enum
{
	VIN_ISP_TYPE,
	ISP_VSE_TYPE
} ConnectionType;

typedef struct
{
	const char *str;
	const char *desc;
	ConnectionType type;
	int status;
} ValidConnection;

static ValidConnection valid_connections[] = {
	{"vo", "VIN online ISP", VIN_ISP_TYPE, 1},
	{"vf", "VIN offline ISP", VIN_ISP_TYPE, 0},
	{"io", "ISP online VSE", ISP_VSE_TYPE, 1},
	{"if", "ISP offline VSE", ISP_VSE_TYPE, 0}};

/**
 * @brief Check if a string is a valid connection (internal helper function)
 * @param str Input sub-string
 * @return Pointer to ValidConnection if valid, NULL otherwise
 */
static const ValidConnection *is_valid_connection(const char *str)
{
	int connection_count = sizeof(valid_connections) / sizeof(ValidConnection);
	for (int i = 0; i < connection_count; i++)
	{
		if (strcmp(str, valid_connections[i].str) == 0)
		{
			return &valid_connections[i];
		}
	}
	return NULL;
}

/**
 * @brief Check if a string contains invalid characters (only v/i/f/o/: allowed)
 * @param str Input string
 * @return 1 if has invalid chars, 0 otherwise
 */
static int has_invalid_chars(const char *str)
{
	while (*str != '\0')
	{
		if (*str != 'v' && *str != 'i' && *str != 'f' && *str != 'o' && *str != ':')
		{
			return 1;
		}
		str++;
	}
	return 0;
}

// Core function: parse channel string
int parse_channel_string(const char *input)
{
	// Check if input is empty
	if (input == NULL || strlen(input) == 0)
	{
		printf("Error: Input string is empty\n");
		return -1;
	}

	// Check for invalid characters
	if (has_invalid_chars(input))
	{
		printf("Error: Input string contains invalid characters (only v/i/f/o/: allowed)\n");
		return -1;
	}

	// Check if string starts or ends with ':'
	int len = strlen(input);
	if (input[0] == ':' || input[len - 1] == ':')
	{
		printf("Error: String starts or ends with colon\n");
		return -1;
	}

	// Check for consecutive colons (e.g., "vo::io")
	for (int i = 0; i < len - 1; i++)
	{
		if (input[i] == ':' && input[i + 1] == ':')
		{
			printf("Error: String contains consecutive colons\n");
			return -1;
		}
	}

	// Split string by ':' and validate each token
	char temp[strlen(input) + 1];
	strcpy(temp, input); // Copy to avoid modifying original string
	char *token = strtok(temp, ":");

	// Mark if each ConnectionType has been used (init to 0: unused)
	int vin_isp_used = 0; // For VIN_ISP_TYPE (vo/vf)
	int isp_vse_used = 0; // For ISP_VSE_TYPE (io/if)
	while (token != NULL)
	{
		// Check if token length is 2 (all valid connections are 2 characters)
		if (strlen(token) != 2)
		{
			printf("Error: Sub-string \"%s\" is invalid (length not 2)\n", token);
			return -1;
		}

		// Check if token is a valid connection
		const ValidConnection *connection = is_valid_connection(token);
		if (connection == NULL)
		{
			printf("Error: Sub-string \"%s\" is not a valid connection\n", token);
			return -1;
		}

		// Check if the ConnectionType has been used before (disallow duplicate type)
		if (connection->type == VIN_ISP_TYPE)
		{
			if (vin_isp_used)
			{
				printf("Error: Duplicate ConnectionType (VIN_ISP_TYPE) - \"%s\" is not allowed\n", token);
				return -1;
			}
			vin_isp_used = 1; // Mark as used
		}
		else if (connection->type == ISP_VSE_TYPE)
		{
			if (isp_vse_used)
			{
				printf("Error: Duplicate ConnectionType (ISP_VSE_TYPE) - \"%s\" is not allowed\n", token);
				return -1;
			}
			isp_vse_used = 1; // Mark as used
		}

		// Print connection description (commented out as original)
		// printf("\"%s\": %s\n", token, connection->desc);

		// Update status variables (last connection overrides previous, but now duplicate type is disallowed)
		if (connection->type == VIN_ISP_TYPE)
		{
			vin_isp_is_online = connection->status;
		}
		else if (connection->type == ISP_VSE_TYPE)
		{
			isp_vse_is_online = connection->status;
		}

		token = strtok(NULL, ":"); // Next token
	}

	return 0; // Success
}
