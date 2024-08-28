#include "daemon.h"
#include <cstdio>
#include <dirent.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <openssl/crypto.h>
#include <sys/stat.h>
#include <sys/types.h>

bool ecs_mode = false;

const std::vector<char> invalid_characters = {
    '&', '|', ';', ':', '$', '*', '?', '<', '>', '!',' ', '\\', '.',']', '[', '+', '\'', '`',
    '~', '}', '{', '"', ')', '('};

const std::string install_path_for_decode_exe =
    "/usr/sbin/credentials_fetcher_utf16_private.exe";

const std::string install_path_for_aws_cli = "/usr/bin/aws";

/**
 * Get list of domain-ips representing a domain
 * @param domain_name Like 'contoso.com'
 * @return - Pair of result and string, 0 if successful and FQDN like win-m744.contoso.com
 */
static std::pair<int, std::vector<std::string>> get_domain_ips( std::string domain_name )
{
    std::vector<std::string> list_of_ips = { "" };
    std::vector<std::string> dummy_ips = { "" };

    std::pair<int, std::string> result = Util::get_dns_ips_list( domain_name );
    list_of_ips = Util::split_string( result.second, '\n' );

    // regex for ip
    std::regex ipregex( "(([0-9]|[1-9][0-9]|1[0-9][0-9]|2[0-4][0-9]|25[0-5])\\.){3}"
                        "([0-9]|[1-9][0-9]|1[0-9][0-9]|2[0-4][0-9]|25[0-5])" );

    for ( const auto& str : list_of_ips )
    {
        if ( !std::regex_match( str, ipregex ) )
        {
            return std::make_pair( -1, dummy_ips );
        }
    }

    return std::make_pair( EXIT_SUCCESS, list_of_ips );
}

/**
 * If the host is domain-joined, the result is of the form EC2AMAZ-Q5VJZQ$@CONTOSO.COM'
 * @param domain_name: Expected domain name as per configuration
 * @return result pair<int, std::string> (error-code - 0 if successful
 *                          string of the form EC2AMAZ-Q5VJZQ$@CONTOSO.COM')
 */
std::pair<int, std::string> get_machine_principal( std::string domain_name,
                                                   creds_fetcher::CF_logger& cf_logger )
{
    std::pair<int, std::string> result = std::make_pair( -1, "" );

    char hostname[HOST_NAME_MAX];
    int status = gethostname( hostname, HOST_NAME_MAX );
    if ( status )
    {
        result.first = status;
        return result;
    }

    std::pair<int, std::string> realm_name_result = Util::get_realm_name();
    if ( realm_name_result.first != 0 )
    {
        return realm_name_result;
    }

    std::pair<int, std::string> domain_name_result = Util::check_domain_name( domain_name );
    if ( domain_name_result.first != 0 )
    {
        return domain_name_result;
    }

    std::string s = std::string( hostname );
    std::string host_name = s.substr( 0, s.find( '.' ) );

    // truncate the hostname to the host name size limit defined by microsoft
    if ( host_name.length() > HOST_NAME_LENGTH_LIMIT )
    {
        cf_logger.logger( LOG_ERR,
                          "WARNING: %s:%d hostname exceeds 15 characters,"
                          "this can cause problems in getting kerberos tickets, please reduce "
                          "hostname length",
                          __func__, __LINE__ );
        host_name = host_name.substr( 0, HOST_NAME_LENGTH_LIMIT );
        std::cerr << Util::getCurrentTime() << '\t'
                  << "INFO: hostname exceeds 15 characters this can "
                     "cause problems in getting kerberos tickets, "
                     "please reduce hostname length"
                  << std::endl;
    }

    /**
     * Machine principal is of the format EC2AMAZ-Q5VJZQ$@CONTOSO.COM'
     */
    result.first = 0;
    result.second = "'" + host_name + "$@'" + realm_name_result.second;

    return result;
}

/**
 * DNS reverse lookup, given IP, return domain name
 * @param domain_name Like 'contoso.com'
 * @return - Pair of result and string, 0 if successful and FQDN like win-m744.contoso.com
 */
static std::pair<int, std::string> get_fqdn_from_domain_ip( std::string domain_ip,
                                                            std::string domain_name )
{

    std::pair<int, std::string> reverse_dns_output = Util::get_FQDNs( domain_ip, domain_name );

    std::vector<std::string> list_of_dc_names;
    list_of_dc_names = Util::split_string( reverse_dns_output.second, '\n' );
    for ( auto fqdn_str : list_of_dc_names )
    {
        if ( fqdn_str.length() == 0 )
        {
            return std::make_pair( EXIT_FAILURE, "" );
        }
        fqdn_str.pop_back(); // Remove trailing .

        /**
         * We can ignore DNS resolution like ip-10-0-0-162.us-west-1.compute.internal
         * since it does not have a domain such as "contoso.com"
         */
        if ( !fqdn_str.empty() && ( fqdn_str.find( domain_name ) != std::string::npos ) )
        {
            return std::make_pair( EXIT_SUCCESS, fqdn_str );
        }
        else
        {
            std::transform( domain_name.begin(), domain_name.end(), domain_name.begin(),
                            []( unsigned char c ) { return std::tolower( c ); } );
            if ( !fqdn_str.empty() && ( fqdn_str.find( domain_name ) != std::string::npos ) )
            {
                return std::make_pair( EXIT_SUCCESS, fqdn_str );
            }
            else
            {
                return std::make_pair( EXIT_FAILURE, "" );
            }
        }
    }

    std::cerr << Util::getCurrentTime() << '\t' << "ERROR: getting FQDN from domain ip"
              << std::endl;
    return std::make_pair( EXIT_FAILURE, "" );
}

/**
 * This function generates the kerberos ticket for the host machine.
 * It uses machine keytab located at /etc/krb5.keytab to generate the ticket.
 * @param cf_daemon - parent daemon object
 * @return error-code - 0 if successful
 */
std::pair<int, std::string> get_machine_krb_ticket( std::string domain_name,
                                                    creds_fetcher::CF_logger& cf_logger )
{
    std::pair<int, std::string> result;

    result = Util::is_hostname_cmd_present();
    if ( result.first != 0 )
    {
        cf_logger.logger( LOG_ERR, result.second.c_str() );
        return result;
    }

    result = Util::is_hostname_cmd_present();
    if ( result.first != 0 )
    {
        cf_logger.logger( LOG_ERR, result.second.c_str() );
        return result;
    }

    result = Util::is_realm_cmd_present();
    if ( result.first != 0 )
    {
        cf_logger.logger( LOG_ERR, result.second.c_str() );
        return result;
    }

    result = Util::is_kinit_cmd_present();
    if ( result.first != 0 )
    {
        cf_logger.logger( LOG_ERR, result.second.c_str() );
        return result;
    }

    result = Util::is_ldapsearch_cmd_present();
    if ( result.first != 0 )
    {
        cf_logger.logger( LOG_ERR, result.second.c_str() );
        return result;
    }

    result = Util::is_decode_exe_present();
    if ( result.first != 0 )
    {
        cf_logger.logger( LOG_ERR, result.second.c_str() );
        return result;
    }

     /**
      ** Machine principal is of the format 'EC2AMAZ-Q5VJZQ$'@CONTOSO.COM
      **/
    std::pair<int, std::string> machine_principal = get_machine_principal( domain_name, cf_logger );
    if ( result.first != 0 )
    {
        std::cerr << "ERROR: " << __func__ << ":" << __LINE__ << " invalid machine principal"
                  << std::endl;
        std::string err_msg = "ERROR: invalid machine principal";
        cf_logger.logger( LOG_ERR, err_msg.c_str() );
        result = std::make_pair( -1, err_msg );
        return result;
    }

    result = Util::execute_kinit_in_domain_joined_case( machine_principal.second );
    if ( result.first != 0 )
    {
        cf_logger.logger( LOG_ERR, result.second.c_str() );
        return result;
    }

    return result;
}

Json::Value get_secret_from_secrets_manager( std::string aws_sm_secret_name )
{
    Json::Value root = Json::nullValue;

    std::string command = std::string( install_path_for_aws_cli ) +
                          std::string( " secretsmanager get-secret-value --secret-id " ) +
                          aws_sm_secret_name + " --query 'SecretString' --output text";
    // /usr/bin/aws secretsmanager get-secret-value --secret-id
    // aws/directoryservices/d-xxxxxxxxxx/gmsa --query 'SecretString' --output text
    std::pair<int, std::string> result = Util::exec_shell_cmd( command );

    if ( result.first == 0 )
    {
        // deserialize json to krb_ticket_info object
        Json::CharReaderBuilder reader;
        std::istringstream string_stream( result.second );
        std::string errors;
        Json::parseFromStream( reader, string_stream, &root, &errors );
    }

    return root;
}

/**
 * This function generates kerberos ticket with user credentials
 * User credentials must have adequate privileges to read gMSA passwords
 * This is an alternative to the machine credentials approach above
 * @param cf_daemon - parent daemon object
 * @return error-code - 0 if successful
 */
std::pair<int, std::string> get_user_krb_ticket( std::string domain_name,
                                                 std::string aws_sm_secret_name,
                                                 creds_fetcher::CF_logger& cf_logger )
{
    std::pair<int, std::string> result;

    std::pair<int, std::string> cmd = Util::exec_shell_cmd( "which kinit" );
    Util::rtrim( cmd.second );
    if ( !Util::check_file_permissions( cmd.second ) )
    {
        std::cerr << Util::getCurrentTime() << '\t' << "ERROR: kinit not found" << std::endl;
        result = std::make_pair( -1, std::string( "ERROR:: kinit not found" ) );
        return result;
    }

    cmd = Util::exec_shell_cmd( "which ldapsearch" );
    Util::rtrim( cmd.second );
    if ( !Util::check_file_permissions( cmd.second ) )
    {
        std::cerr << Util::getCurrentTime() << '\t' << "ERROR: ldapsearch not found" << std::endl;
        result = std::make_pair( -1, "ERROR:: ldapsearch not found" );
        return result;
    }

    if ( !Util::check_file_permissions( std::string( install_path_for_decode_exe ) ) )
    {
        result = std::make_pair( -1, "ERROR:: decode.exe not found" );
        return result;
    }

    if ( !Util::check_file_permissions( std::string( install_path_for_aws_cli ) ) )
    {
        result = std::make_pair( -1, "ERROR:: AWS CLI not found" );
        return result;
    }

    std::string username = "";
    std::string password = "";
    Json::Value root = get_secret_from_secrets_manager( aws_sm_secret_name );

    std::string distinguished_name = "";
    if ( root != Json::nullValue )
    {
        distinguished_name = root["distinguishedName"].asString();
        username = root["username"].asString();
        password = root["password"].asString();
    }
    std::cerr << "[Optional] DN from Secrets Manager = " << distinguished_name << std::endl;

    std::transform( domain_name.begin(), domain_name.end(), domain_name.begin(),
                    []( unsigned char c ) { return std::toupper( c ); } );

    // kinit using api interface
    char* kinit_argv[3];

    kinit_argv[0] = (char*)"my_kinit";
    username = username + "@" + domain_name;
    kinit_argv[1] = (char*)username.c_str();
    kinit_argv[2] = (char*)password.c_str();
    int ret = my_kinit_main( 2, kinit_argv );
#if 0
    /* The old way */
    std::string kinit_cmd = "echo '"  + password +  "' | kinit -V " + username + "@" +
                            domain_name;
    username = "xxxx";
    password = "xxxx";
    result = Util::exec_shell_cmd( kinit_cmd );
    kinit_cmd = "xxxx";
    return result.first;
#endif

    Util::clearString( username );
    Util::clearString( password );

    result = std::make_pair( ret, distinguished_name );

    return result;
}

/**
 * This function generates kerberos ticket with user with access to gMSA password credentials
 * User credentials must have adequate privileges to read gMSA passwords
 * This is an alternative to the machine credentials approach above
 * @param cf_daemon - parent daemon object
 * @return error-code - 0 if successful
 */
std::pair<int, std::string> get_domainless_user_krb_ticket( std::string domain_name,
                                                            std::string username,
                                                            std::string password,
                                                            creds_fetcher::CF_logger& cf_logger )
{
    std::pair<int, std::string> result;

    std::pair<int, std::string> cmd = Util::exec_shell_cmd( "which kinit" );
    Util::rtrim( cmd.second );
    if ( !Util::check_file_permissions( cmd.second ) )
    {
        std::cerr << Util::getCurrentTime() << '\t' << "ERROR: kinit not found" << std::endl;
        result = std::make_pair( -1, "ERROR: kinit not found" );
        return result;
    }

    cmd = Util::exec_shell_cmd( "which ldapsearch" );
    Util::rtrim( cmd.second );
    if ( !Util::check_file_permissions( cmd.second ) )
    {
        std::cerr << Util::getCurrentTime() << '\t' << "ERROR: ldapsearch not found" << std::endl;
        result = std::make_pair( -1, "ERROR: ldapsearch not found" );
        return result;
    }

    std::transform( domain_name.begin(), domain_name.end(), domain_name.begin(),
                    []( unsigned char c ) { return std::toupper( c ); } );

    // kinit using api interface
    char* kinit_argv[3];

    kinit_argv[0] = (char*)"my_kinit";
    username = username + "@" + domain_name;
    kinit_argv[1] = (char*)username.c_str();
    kinit_argv[2] = (char*)password.c_str();
    int ret = my_kinit_main( 2, kinit_argv );
    Util::clearString( username );
    Util::clearString( password );

    result = std::make_pair( ret, "" );

    return result;
}

/**
 * base64_decode - Decodes base64 encoded string
 * @param password - base64 encoded password
 * @param base64_decode_len - Length after decode
 * @return buffer with base64 decoded contents
 */
static uint8_t* base64_decode( const std::string& password, gsize* base64_decode_len )
{
    if ( base64_decode_len == nullptr || password.empty() )
    {
        return nullptr;
    }

    *base64_decode_len = 0;
    guchar* result = g_base64_decode( password.c_str(), base64_decode_len );
    if ( result == nullptr || *base64_decode_len <= 0 )
    {
        return nullptr;
    }

    void* secure_mem = OPENSSL_malloc( *base64_decode_len );
    if ( secure_mem == nullptr )
    {
        g_free( result );
        return nullptr;
    }

    memcpy( secure_mem, result, *base64_decode_len );

    memset( result, 0, *base64_decode_len );
    g_free( result );

    /**
     * secure_mem must be freed later
     */
    return (uint8_t*)secure_mem;
}

static std::pair<size_t, void*> find_password( std::string ldap_search_result )
{
    size_t base64_decode_len = 0;
    std::vector<std::string> results;

    std::string password = std::string( "msDS-ManagedPassword::" );
    results = Util::split_string( ldap_search_result, '#' );
    bool password_found = false;
    for ( auto& result : results )
    {
        auto found = result.find( password );
        if ( found != std::string::npos )
        {
            found += password.length();
            password = result.substr( found + 1, result.length() );
            // std::cerr << "Password = " << password << std::endl;
            password_found = true;
            break;
        }
    }

    uint8_t* blob_base64_decoded = nullptr;
    if ( password_found )
    {
        blob_base64_decoded = base64_decode( password, &base64_decode_len );
        if ( blob_base64_decoded == nullptr )
        {
            std::cerr << Util::getCurrentTime() << '\t' << "ERROR: base64 buffer is null"
                      << std::endl;
            return std::make_pair( 0, nullptr );
        }
    }

    return std::make_pair( base64_decode_len, blob_base64_decoded );
}

/**
 * UTF-16 diagnostic: Test utf16 capability
 * @return - true (pass) or false (fail)
 */
/*int test_utf16_decode()
{
    const char* test_msds_managed_password =
        "msDS-ManagedPassword:: "
        "AQAAACIBAAAQAAAAEgEaAciMhCofvo1R4kkVYm79aRysUcOs7NhhHvO"
        "exhNTV9KXAn1v8AYMN1lMC/V6W0dZVrQRpGZ/EvWi33Lq2xoR5ANuJf623JQRj3pMZQBqQLRjRoPn"
        "UJYY8H74aVysf0t+1M0moLkm0IPSCB52Mm0CC9flTT0D9KZV2Mvf4FpgvYpYoOQvUmd0UOV72Tk/d"
        "leM8zTWjRL5ccfzwt5p8akMEl6W0RPj1pDbqxtbpJFQiLQd7HRlSkYPeBKDB9r6CItrQTo8j+pgJf"
        "B4+wVbOUZuMXrKkDVh8XUOUBdGhznntRWnDM2DhwBoFEisBr133Vo8aRcedYqwNj/LEsrimEJaeuY"
        "AAAQCCBrPFgAABKQ3Z84WAAA= #";

    uint8_t test_password_buf[1024];

    const uint8_t test_gmsa_utf8_password[] = {
        0xE8, 0xB3, 0x88, 0xE2, 0xAA, 0x84, 0xEB, 0xB8, 0x9F, 0xE5, 0x86, 0x8D, 0xE4, 0xA7, 0xA2,
        0xE6, 0x88, 0x95, 0xEF, 0xB5, 0xAE, 0xE1, 0xB1, 0xA9, 0xE5, 0x86, 0xAC, 0xEA, 0xB3, 0x83,
        0xEF, 0xBF, 0xBD, 0xE1, 0xB9, 0xA1, 0xE9, 0xBB, 0xB3, 0xE1, 0x8F, 0x86, 0xE5, 0x9D, 0x93,
        0xE9, 0x9F, 0x92, 0xE7, 0xB4, 0x82, 0xEF, 0x81, 0xAF, 0xE0, 0xB0, 0x86, 0xE5, 0xA4, 0xB7,
        0xE0, 0xAD, 0x8C, 0xE7, 0xAB, 0xB5, 0xE4, 0x9D, 0x9B, 0xE5, 0x99, 0x99, 0xE1, 0x86, 0xB4,
        0xE6, 0x9A, 0xA4, 0xE1, 0x89, 0xBF, 0xEA, 0x8B, 0xB5, 0xE7, 0x8B, 0x9F, 0xEF, 0xBF, 0xBD,
        0xE1, 0x84, 0x9A, 0xCF, 0xA4, 0xE2, 0x95, 0xAE, 0xEB, 0x9B, 0xBE, 0xE9, 0x93, 0x9C, 0xE8,
        0xBC, 0x91, 0xE4, 0xB1, 0xBA, 0x65, 0xE4, 0x81, 0xAA, 0xE6, 0x8E, 0xB4, 0xE8, 0x8D, 0x86,
        0xE5, 0x83, 0xA7, 0xE1, 0xA2, 0x96, 0xE7, 0xBB, 0xB0, 0xE6, 0xA7, 0xB8, 0xEA, 0xB1, 0x9C,
        0xE4, 0xAD, 0xBF, 0xED, 0x91, 0xBE, 0xE2, 0x9B, 0x8D, 0xEB, 0xA6, 0xA0, 0xED, 0x80, 0xA6,
        0xED, 0x8A, 0x83, 0xE1, 0xB8, 0x88, 0xE3, 0x89, 0xB6, 0xC9, 0xAD, 0xED, 0x9C, 0x8B, 0xE4,
        0xB7, 0xA5, 0xCC, 0xBD, 0xEA, 0x9B, 0xB4, 0xF0, 0xA5, 0x9F, 0x8B, 0xE5, 0xAB, 0xA0, 0xEB,
        0xB5, 0xA0, 0xE5, 0xA2, 0x8A, 0xEE, 0x92, 0xA0, 0xE5, 0x88, 0xAF, 0xE7, 0x91, 0xA7, 0xEE,
        0x95, 0x90, 0xEF, 0xBF, 0xBD, 0xE3, 0xBC, 0xB9, 0xE5, 0x9D, 0xB6, 0xEF, 0x8E, 0x8C, 0xED,
        0x98, 0xB4, 0xE1, 0x8A, 0x8D, 0xE7, 0x87, 0xB9, 0xEF, 0x8F, 0x87, 0xEF, 0xBF, 0xBD, 0xEF,
        0x85, 0xA9, 0xE0, 0xB2, 0xA9, 0xE5, 0xB8, 0x92, 0xED, 0x86, 0x96, 0xEE, 0x8C, 0x93, 0xE9,
        0x83, 0x96, 0xEA, 0xAF, 0x9B, 0xE5, 0xAC, 0x9B, 0xE9, 0x86, 0xA4, 0xE8, 0xA1, 0x90, 0xE1,
        0xB6, 0xB4, 0xE7, 0x93, 0xAC, 0xE4, 0xA9, 0xA5, 0xE0, 0xBD, 0x86, 0xE1, 0x89, 0xB8, 0xDE,
        0x83, 0xEF, 0xAB, 0x9A, 0xE8, 0xAC, 0x88, 0xE4, 0x85, 0xAB, 0xE3, 0xB0, 0xBA, 0xEE, 0xAA,
        0x8F, 0xE2, 0x95, 0xA0, 0xE7, 0xA3, 0xB0, 0xD7, 0xBB, 0xE3, 0xA5, 0x9B, 0xE6, 0xB9, 0x86,
        0xE7, 0xA8, 0xB1, 0xE9, 0x83, 0x8A, 0xE6, 0x84, 0xB5, 0xE7, 0x97, 0xB1, 0xE5, 0x80, 0x8E,
        0xE4, 0x98, 0x97, 0xE3, 0xA6, 0x87, 0xEB, 0x97, 0xA7, 0xEA, 0x9C, 0x95, 0xEC, 0xB4, 0x8C,
        0xE8, 0x9E, 0x83, 0xE6, 0xA0, 0x80, 0xE4, 0xA0, 0x94, 0xDA, 0xAC, 0xE7, 0x9E, 0xBD, 0xE5,
        0xAB, 0x9D, 0xE6, 0xA4, 0xBC, 0xE1, 0xB8, 0x97, 0xE8, 0xA9, 0xB5, 0xE3, 0x9A, 0xB0, 0xEC,
        0xAC, 0xBF, 0xEC, 0xA8, 0x92, 0xE9, 0xA3, 0xA2, 0xE5, 0xA9, 0x82, 0xEE, 0x99, 0xBA };

    std::string decoded_password_file = "./decoded_password_file";

    std::pair<size_t, void*> base64_decoded_password_blob =
        find_password( test_msds_managed_password );
    if ( base64_decoded_password_blob.first == 0 || base64_decoded_password_blob.second == nullptr )
    {
        return EXIT_FAILURE;
    }

    struct stat st;
    std::string decode_exe_path;

    if ( stat( install_path_for_decode_exe, &st ) == 0 )
    {
        decode_exe_path = install_path_for_decode_exe;
    }
    else
    {
        // For test during rpmbuild
        decode_exe_path = "./decode.exe";
    }

    // Use decode.exe in build directory
    std::string decode_cmd = decode_exe_path + std::string( "  > " ) + decoded_password_file;
    creds_fetcher::blob_t* blob = ( (creds_fetcher::blob_t*)base64_decoded_password_blob.second );
    FILE* fp = popen( decode_cmd.c_str(), "w" );
    if ( fp == nullptr )
    {
        std::cerr << Util::getCurrentTime() << '\t' << "Self test failed" << std::endl;
        OPENSSL_cleanse( base64_decoded_password_blob.second, base64_decoded_password_blob.first );
        OPENSSL_free( base64_decoded_password_blob.second );
        return EXIT_FAILURE;
    }
    fwrite( blob->current_password, 1, GMSA_PASSWORD_SIZE, fp );
    if ( pclose( fp ) < 0 )
    {
        std::cerr << Util::getCurrentTime() << '\t' << "Self test failed" << std::endl;
        OPENSSL_cleanse( base64_decoded_password_blob.second, base64_decoded_password_blob.first );
        OPENSSL_free( base64_decoded_password_blob.second );
        return EXIT_FAILURE;
    }

    fp = fopen( decoded_password_file.c_str(), "rb" );
    if ( fp == nullptr )
    {
        std::cerr << Util::getCurrentTime() << '\t'  << "Self test failed" << std::endl;
        OPENSSL_cleanse( base64_decoded_password_blob.second, base64_decoded_password_blob.first );
        OPENSSL_free( base64_decoded_password_blob.second );
        return EXIT_FAILURE;
    }
    fread( test_password_buf, 1, GMSA_PASSWORD_SIZE, fp );

    if ( memcmp( test_gmsa_utf8_password, test_password_buf, GMSA_PASSWORD_SIZE ) == 0 )
    {
        // utf16->utf8 conversion works as expected
        std::cerr << Util::getCurrentTime() << '\t'  << "Self test is successful" << std::endl;
        OPENSSL_cleanse( base64_decoded_password_blob.second, base64_decoded_password_blob.first );
        OPENSSL_free( base64_decoded_password_blob.second );
        unlink( decoded_password_file.c_str() );
        return EXIT_SUCCESS;
    }

    std::cerr << Util::getCurrentTime() << '\t'  << "Self test failed" << std::endl;
    OPENSSL_cleanse( base64_decoded_password_blob.second, base64_decoded_password_blob.first );
    OPENSSL_free( base64_decoded_password_blob.second );
    unlink( decoded_password_file.c_str() );
    return EXIT_FAILURE;
}*/

/**
 * This function fetches the gmsa password and creates a krb ticket
 * It uses the existing krb ticket of machine to run ldap query over
 * kerberos and do the appropriate UTF decoding.
 *
 * @param domain_name - Like 'contoso.com'
 * @param gmsa_account_name - Like 'webapp01'
 * @param krb_cc_name - Like '/var/credentials_fetcher/krb_dir/krb5_cc'
 * @param cf_logger - log to systemd daemon
 * @return result code and kinit log, 0 if successful, -1 on failure
 */
std::pair<int, std::string> get_gmsa_krb_ticket( std::string domain_name,
                                                 const std::string& gmsa_account_name,
                                                 const std::string& krb_cc_name,
                                                 creds_fetcher::CF_logger& cf_logger )
{
    std::string domain_controller_gmsa( ENV_CF_DOMAIN_CONTROLLER );
    std::vector<std::string> results;

    if ( domain_name.empty() || gmsa_account_name.empty() )
    {
        cf_logger.logger( LOG_ERR, "ERROR: %s:%d null args", __func__, __LINE__ );
        std::string err_msg = std::string( "domain_name " + domain_name + " or gmsa_account_name " +
                                           gmsa_account_name + " is empty" );
        return std::make_pair( -1, err_msg );
    }

    results = Util::split_string( domain_name, '.' );
    std::string base_dn; /* Distinguished name */
    for ( auto& result : results )
    {
        base_dn += "DC=" + result + ",";
    }
    base_dn.pop_back(); // Remove last comma

    std::string fqdn;
    fqdn = retrieve_variable_from_ecs_config( domain_controller_gmsa );

    std::list<std::string> fqdn_list;
    if ( fqdn.empty() && !Util::is_ecs_mode() )
    {
        std::pair<int, std::vector<std::string>> domain_ips = get_domain_ips( domain_name );
        // TBD:: Use nslookup -type=any _ldap._tcp.dc._msdcs.CONTOSO.COM | grep msdcs | awk '{print
        // $NF}' | sed 's/.$//g'
        if ( domain_ips.first != 0 )
        {
            std::string err_msg = "ERROR: Cannot resolve domain IPs for " + domain_name;
            cf_logger.logger( LOG_ERR, err_msg.c_str() );
            std::cerr << Util::getCurrentTime() << '\t' << err_msg << std::endl;
            return std::make_pair( -1, err_msg );
        }

        for ( auto domain_ip : domain_ips.second )
        {
            auto fqdn_result = get_fqdn_from_domain_ip( domain_ip, domain_name );
            if ( fqdn_result.first == 0 )
            {
                fqdn_list.push_back( fqdn_result.second );
            }
        }
    }
    else if ( !fqdn.empty() )
    {
        fqdn_list.push_back( fqdn );
    }
    else
    {
        return std::make_pair( -1, std::string( "ERROR: FQDN of DC is not available" ) );
    }

    /**
     * ldapsearch -H ldap://<fqdn> -b 'CN=webapp01,CN=Managed Service
     *   Accounts,DC=contoso,DC=com' -s sub  "(objectClass=msDs-GroupManagedServiceAccount)"
     *   msDS-ManagedPassword
     */
    std::string gmsa_ou = std::string( ",CN=Managed Service Accounts," );
    std::string env_base_dn = retrieve_variable_from_ecs_config( ENV_CF_GMSA_BASE_DN );
    if ( getenv( ENV_CF_GMSA_OU ) != NULL )
    {
        gmsa_ou = std::string( "," ) + std::string( getenv( ENV_CF_GMSA_OU ) ) + std::string( "," );
    }
    else if ( getenv( ENV_CF_GMSA_BASE_DN ) != NULL )
    {
        env_base_dn = std::string( getenv( ENV_CF_GMSA_BASE_DN ) );
    }

    std::string secret_name = retrieve_variable_from_ecs_config( ENV_CF_GMSA_SECRET_NAME );
    Json::Value root = get_secret_from_secrets_manager( secret_name );
    std::string distinguished_name = root["distinguishedName"].asString();
    if ( !distinguished_name.empty() )
    {
        env_base_dn = distinguished_name;
        if ( env_base_dn.find( "msds-ManagedPassword" ) == std::string::npos )
        {
            env_base_dn = env_base_dn + std::string( " msds-ManagedPassword" );
        }
    }

    bool found_valid_fqdn = false; // Valid if ldapsearch succeeds with a valid FQDN
    std::pair<int, std::string> ldap_search_result;

    for ( auto fqdn : fqdn_list )
    {
        std::string cmd;
        if ( !env_base_dn.empty() )
        {
            cmd = std::string( "ldapsearch -LLL -Y GSSAPI -H ldap://" ) + fqdn;
            cmd += std::string( " -b " ) + env_base_dn + std::string( " msds-ManagedPassword" );
        }
        else
        {
            cmd = std::string( "ldapsearch -H ldap://" ) + fqdn;
            cmd += std::string( " -b 'CN=" ) + gmsa_account_name + gmsa_ou + base_dn +
                   std::string( "'" ) +
                   std::string( " -s sub  '(objectClass=msDs-GroupManagedServiceAccount)' "
                                " msDS-ManagedPassword" );
        }

        cf_logger.logger( LOG_INFO, "%s", cmd.c_str() );
        std::cerr << Util::getCurrentTime() << '\t' << "INFO: " << cmd << std::endl;
        std::cerr << cmd << std::endl;

        for ( int i = 0; i < 2; i++ )
        {
            ldap_search_result = Util::exec_shell_cmd( cmd );
            // Add retry, ldapsearch seems to fail and then succeed on retry
            if ( ldap_search_result.first != 0 )
            {
                std::string err_msg =  std::string("ERROR: ldapsearch failed with FQDN = ")  + fqdn;
                cf_logger.logger( LOG_ERR, err_msg.c_str() );
                std::cerr << err_msg << std::endl;
                err_msg =  Util::getCurrentTime() +  std::string("ERROR: ldapsearch failed to get gMSA credentials: " + ldap_search_result.second);
                cf_logger.logger( LOG_ERR, err_msg.c_str() );
                std::cerr << err_msg << std::endl;
            }
            else
            {
                cf_logger.logger( LOG_INFO, "INFO: %s:%d ldapsearch succeeded with FQDN = %s",
                                  __func__, __LINE__, fqdn );
                std::cerr << "INFO: ldapsearch succeeded with FQDN = " << fqdn << std::endl;
                found_valid_fqdn = true;
                break;
            }
        }
    }
    fqdn_list.clear();

    if ( !found_valid_fqdn ) // ldapsearch did not work in any FQDN
    {
        return std::make_pair( -1, std::string( "" ) );
    }

    std::pair<size_t, void*> password_found_result = find_password( ldap_search_result.second );

    if ( password_found_result.first == 0 || password_found_result.second == nullptr )
    {
        std::cerr << Util::getCurrentTime() << '\t' << "ERROR: Password not found" << std::endl;
        return std::make_pair( -1, std::string( "" ) );
    }

    creds_fetcher::blob_t* blob = ( (creds_fetcher::blob_t*)password_found_result.second );
    auto* blob_password = (uint8_t*)blob->current_password;

    std::transform( domain_name.begin(), domain_name.end(), domain_name.begin(),
                    []( unsigned char c ) { return std::toupper( c ); } );
    std::string default_principal = "'" + gmsa_account_name + "$'" + "@" + domain_name;

    /* Pipe password to the utf16 decoder and kinit */
    std::string kinit_cmd = std::string( "dotnet " ) + std::string( install_path_for_decode_exe ) +
                            std::string( " | kinit " ) + std::string( " -c " ) + krb_cc_name +
                            " -V " + default_principal;
    std::cerr << Util::getCurrentTime() << '\t' << "INFO:" << kinit_cmd << std::endl;
    FILE* fp = popen( kinit_cmd.c_str(), "w" );
    if ( fp == nullptr )
    {
        perror( "kinit failed" );
        OPENSSL_cleanse( password_found_result.second, password_found_result.first );
        OPENSSL_free( password_found_result.second );
        cf_logger.logger( LOG_ERR, "ERROR: %s:%d kinit failed", __func__, __LINE__ );
        std::cerr << Util::getCurrentTime() << '\t' << "ERROR: kinit failed" << std::endl;
        return std::make_pair( -1, std::string( "kinit failed" ) );
    }
    fwrite( blob_password, 1, GMSA_PASSWORD_SIZE, fp );
    int error_code = pclose( fp );

    // kinit output
    std::cerr << Util::getCurrentTime() << '\t' << "INFO: kinit return value = " << error_code
              << std::endl;

    OPENSSL_cleanse( password_found_result.second, password_found_result.first );
    OPENSSL_free( password_found_result.second );

    return std::make_pair( error_code, krb_cc_name );
}

/**
 * Parses the string that is a result of the klist command for the ticket experation date and time
 * @param klist_ticket_info  - String output of the klist command to parse
 * @return - returns the date and time of the ticket experation otherwise an empty string
 */
std::string get_ticket_expiration( std::string klist_ticket_info )
{
    /*
     * Ticket cache: KEYRING:persistent:1000:1000
     * Default principal: admin@CUSTOMERTEST.LOCAL

    * Valid starting       Expires              Service principal
        * 12/04/2023 19:39:06  12/05/2023 05:39:06  krbtgt/CUSTOMERTEST.LOCAL@CUSTOMERTEST.LOCAL
                                                                               * renew until
    12/11/2023 19:39:04
        */

    std::string any_regex( ".+" );
    std::string day_regex( "[0-9]{2}" );
    std::string month_regex( "[0-9]{2}" );
    std::string year_in_four_digits_regex( "[0-9]{4}" );
    std::string year_in_two_digits_regex( "[0-9]{2}" );
    std::string time_regex( "([0-9]{2}:[0-9]{2}:[0-9]{2})" );
    std::string separator_regex( "[/]{1}" );
    std::string space_regex( "[ ]+" );
    std::string left_paren_regex( "(" );
    std::string right_paren_regex( ")" );
    std::string krbtgt_regex( "krbtgt" );

    std::string date_regex = left_paren_regex + day_regex + separator_regex + month_regex +
                             separator_regex + year_in_four_digits_regex + right_paren_regex;

    /* 12/04/2023 19:39:06  12/05/2023 05:39:06  krbtgt/CUSTOMERTEST.LOCAL@CUSTOMERTEST.LOCAL */
    std::string expires_regex = date_regex + space_regex + time_regex + space_regex + date_regex +
                                space_regex + time_regex + space_regex + krbtgt_regex;

    std::string regex_pattern( expires_regex );
    std::regex pattern( expires_regex );
    std::smatch expires_match;

    if ( !std::regex_search( klist_ticket_info, expires_match, pattern ) )
    {
        // Retry with 2 digit year
        /* 12/04/23 21:58:51  12/05/23 07:58:51  krbtgt/CUSTOMERTEST.LOCAL@CUSTOMERTEST.LOCAL */
        date_regex = left_paren_regex + day_regex + separator_regex + month_regex +
                     separator_regex + year_in_two_digits_regex + right_paren_regex;
        expires_regex = date_regex + space_regex + time_regex + space_regex + date_regex +
                        space_regex + time_regex + space_regex + krbtgt_regex;
        pattern = expires_regex;
        if ( !std::regex_search( klist_ticket_info, expires_match, pattern ) )
        {
            std::cerr << "Unable to parse klist for ticket expiration: " << klist_ticket_info
                      << std::endl;
            return std::string( "" );
        }
    }

    /*
     * From example above:
     * 12/04/2023
     * 19:39:06
     * 12/05/2023
     * 05:39:06
     */
    std::string klist_valid_date;
    std::string klist_valid_time;
    std::string klist_expires_date;
    std::string klist_expires_time;
    for ( auto it = expires_match.cbegin(); it != expires_match.cend(); it++ )
    {
        // First one is the full string
        if ( it != expires_match.cbegin() )
        {
            if ( klist_valid_date.empty() )
            {
                klist_valid_date = *it;
                continue;
            }
            if ( klist_valid_time.empty() )
            {
                klist_valid_time = *it;
                continue;
            }
            if ( klist_expires_date.empty() )
            {
                klist_expires_date = *it;
                continue;
            }
            if ( klist_expires_time.empty() )
            {
                klist_expires_time = *it;
                continue;
            }
        }
    }

    return klist_expires_date + " " + klist_expires_time;
}

/**
 * Checks if the given ticket needs renewal or recreation
 * @param krb_cc_name  - Like '/var/credentials_fetcher/krb_dir/krb5_cc'
 * @return - is renewal needed - true or false
 */

bool is_ticket_ready_for_renewal( creds_fetcher::krb_ticket_info* krb_ticket_info )
{
    std::string cmd = "export KRB5CCNAME=" + krb_ticket_info->krb_file_path + " &&  klist";
    std::pair<int, std::string> krb_ticket_info_result = Util::exec_shell_cmd( cmd );
    if ( krb_ticket_info_result.first != 0 )
    {
        // we need to check if meta file exists to recreate the ticket
        std::cerr << Util::getCurrentTime() << '\t' << "ERROR: klist failed for command " << cmd
                  << std::endl;
        return false;
    }

    std::vector<std::string> results;

    results = Util::split_string( krb_ticket_info_result.second, '#' );
    std::string renew_until = "renew until";
    bool is_ready_for_renewal = false;

    for ( auto& result : results )
    {
        auto found = result.find( renew_until );
        if ( found != std::string::npos )
        {
            std::string renewal_date_time;

            renewal_date_time = get_ticket_expiration( result );

            char renewal_date[80];
            char renewal_time[80];

            sscanf( renewal_date_time.c_str(), "%79s %79s", renewal_date, renewal_time );

            renew_until = std::string( renewal_date ) + " " + std::string( renewal_time );
            // trim extra spaces
            Util::ltrim( renew_until );
            Util::rtrim( renew_until );

            // next renewal time for the ticket
            struct tm tm;

            // if the string is not date time format, return false
            if ( strptime( renew_until.c_str(), "%m/%d/%Y %T", &tm ) == NULL )
                return false;

            std::time_t next_renewal_time = mktime( &tm );

            // get the current system time
            std::time_t t = std::time( NULL );
            std::tm* now = std::localtime( &t );
            std::time_t current_time = mktime( now );

            // calculate the time difference in hours
            double hours = std::difftime( next_renewal_time, current_time ) / SECONDS_IN_HOUR;

            // check of the ticket need to be renewed
            if ( hours <= RENEW_TICKET_HOURS )
            {
                is_ready_for_renewal = true;
            }
            break;
        }
    }

    return is_ready_for_renewal;
}

/**
 * This function does the ticket renewal in domainless mode.
 * @param krb_files_dir
 * @param domain_name
 * @param username
 * @param password
 */
std::list<std::string> renew_kerberos_tickets_domainless( std::string krb_files_dir,
                                                          std::string domain_name,
                                                          std::string username,
                                                          std::string password,
                                                          creds_fetcher::CF_logger& cf_logger )
{
    std::list<std::string> renewed_krb_ticket_paths;
    // identify the metadata files in the krb directory
    std::vector<std::string> metadatafiles = get_meta_data_file_paths( krb_files_dir );

    // read the information of service account from the files
    for ( auto file_path : metadatafiles )
    {
        std::list<creds_fetcher::krb_ticket_info*> krb_ticket_info_list =
            read_meta_data_json( file_path );

        // refresh the kerberos tickets for the service accounts, if tickets ready for
        // renewal
        for ( auto krb_ticket : krb_ticket_info_list )
        {
            std::string domainlessuser = krb_ticket->domainless_user;
            if ( !username.empty() && username == domainlessuser )
            {
                std::string renewed_ticket_path =
                    renew_gmsa_ticket( krb_ticket, domain_name, username, password, cf_logger );

                if ( !renewed_krb_ticket_paths.empty() )
                {
                    renewed_krb_ticket_paths.push_back( renewed_ticket_path );
                }
            }
        }
    }

    return renewed_krb_ticket_paths;
}

/**
 * get metadata file info
 * @param krbdir - location for kerberos directory
 */
std::vector<std::string> get_meta_data_file_paths( std::string krbdir )
{
    // identify the metadata files in the krb directory
    std::vector<std::string> metadatafiles;
    for ( std::filesystem::recursive_directory_iterator end, dir( krbdir ); dir != end; ++dir )
    {
        auto path = dir->path();
        if ( std::filesystem::is_regular_file( path ) )
        {
            // find the file with metadata extension
            std::string filename = path.filename().string();
            if ( !filename.empty() && filename.find( "_metadata" ) != std::string::npos )
            {
                std::string filepath = path.parent_path().string() + "/" + filename;
                metadatafiles.push_back( filepath );
            }
        }
    }
    return metadatafiles;
}

/**
 * renew gmsa kerberos tickets
 * @param krb_ticket_info - kerberos ticket info
 * @param domain_name
 * @param username
 * @param password
 * @param cf_logger - credentials fetcher logger
 */
std::string renew_gmsa_ticket( creds_fetcher::krb_ticket_info* krb_ticket, std::string domain_name,
                               std::string username, std::string password,
                               creds_fetcher::CF_logger& cf_logger )
{
    std::string renewed_krb_ticket_path;
    std::pair<int, std::string> gmsa_ticket_result;
    std::string krb_cc_name = krb_ticket->krb_file_path;

    // gMSA kerberos ticket generation needs to have ldap over kerberos
    // if the ticket exists for the machine/user already reuse it for getting gMSA password else
    // retry the ticket creation again after generating user/machine kerberos ticket
    int num_retries = 2;
    for ( int i = 0; i < num_retries; i++ )
    {
        gmsa_ticket_result = get_gmsa_krb_ticket(
            krb_ticket->domain_name, krb_ticket->service_account_name, krb_cc_name, cf_logger );
        if ( gmsa_ticket_result.first != 0 )
        {
            if ( i == 0 )
            {
                cf_logger.logger( LOG_WARNING,
                                  "WARNING: Cannot get gMSA krb ticket "
                                  "because of expired user/machine ticket, "
                                  "will be retried automatically, service_account_name = %s",
                                  krb_ticket->service_account_name.c_str() );
            }
            else
            {
                cf_logger.logger( LOG_ERR, "ERROR: Cannot get gMSA krb ticket using account %s",
                                  krb_ticket->service_account_name.c_str() );

                std::cerr << Util::getCurrentTime() << '\t'
                          << "ERROR: Cannot get gMSA krb ticket using account" << std::endl;
            }
            // if tickets are created in domainless mode
            std::string domainless_user = krb_ticket->domainless_user;
            if ( !domainless_user.empty() && domainless_user == username )
            {
                std::pair<int, std::string> status =
                    get_domainless_user_krb_ticket( domain_name, username, password, cf_logger );

                if ( status.first < 0 )
                {
                    cf_logger.logger( LOG_ERR, "ERROR %d: Cannot get user krb ticket", status );
                    std::cerr << Util::getCurrentTime() << '\t'
                              << "ERROR: Cannot get user krb ticket" << std::endl;
                }
            }
            else
            {
                break;
            }
        }
        else
        {
            renewed_krb_ticket_path = krb_cc_name;
            i++;
        }
    }

    return renewed_krb_ticket_path;
}

/**
 * delete kerberos ticket corresponding to lease id
 * @param krb_files_dir - path to kerberos directory
 * @param lease_id - lease_id associated to kerberos tickets
 * @return - vector of kerberos deleted paths
 */
std::vector<std::string> delete_krb_tickets( std::string krb_files_dir, std::string lease_id )
{
    std::vector<std::string> delete_krb_ticket_paths;
    if ( lease_id.empty() || krb_files_dir.empty() )
        return delete_krb_ticket_paths;

    std::string krb_tickets_path = krb_files_dir + "/" + lease_id;

    DIR* curr_dir;
    struct dirent* file;
    // open the directory
    curr_dir = opendir( krb_tickets_path.c_str() );
    try
    {
        if ( curr_dir )
        {
            while ( ( file = readdir( curr_dir ) ) != NULL )
            {
                std::string filename = file->d_name;
                if ( !filename.empty() && filename.find( "_metadata" ) != std::string::npos )
                {
                    std::string file_path = krb_tickets_path + "/" + filename;
                    std::list<creds_fetcher::krb_ticket_info*> krb_ticket_info_list =
                        read_meta_data_json( file_path );

                    for ( auto krb_ticket : krb_ticket_info_list )
                    {
                        std::string krb_file_path = krb_ticket->krb_file_path;
                        std::string cmd = "export KRB5CCNAME=" + krb_file_path + " && kdestroy";

                        std::pair<int, std::string> krb_ticket_destroy_result =
                            Util::exec_shell_cmd( cmd );
                        if ( krb_ticket_destroy_result.first == 0 )
                        {
                            delete_krb_ticket_paths.push_back( krb_file_path );
                        }
                        else
                        {
                            // log ticket deletion failure
                            std::cerr << Util::getCurrentTime() << '\t'
                                      << "Delete kerberos ticket "
                                         "failed" +
                                             krb_file_path
                                      << std::endl;
                        }
                    }
                }
            }
            // close directory
            closedir( curr_dir );

            // finally delete lease file and directory
            std::filesystem::remove_all( krb_tickets_path );
        }
    }
    catch ( ... )
    {
        std::cerr << Util::getCurrentTime() << '\t'
                  << "Delete kerberos ticket "
                     "failed"
                  << std::endl;
        closedir( curr_dir );
        return delete_krb_ticket_paths;
    }
    return delete_krb_ticket_paths;
}

std::string retrieve_variable_from_ecs_config( std::string ecs_variable_name )
{
    const char* ecs_config_file_name = "/etc/ecs/ecs.config";

    std::ifstream config_file( ecs_config_file_name );
    std::string line;
    std::vector<std::string> results;

    while ( std::getline( config_file, line ) )
    {
        results = Util::split_string( line, '=' );

        if ( results.empty() || results.size() != 2 )
        {
            std::cerr << Util::getCurrentTime() << '\t' << "invalid configuration format"
                      << std::endl;
            return "";
        }

        std::string key = results[0];
        std::string value = results[1];

        Util::rtrim( key );
        Util::ltrim( value );

        if ( key.compare( ENV_CF_GMSA_BASE_DN ) == 0 && ecs_variable_name.compare( key ) == 0 )
        {
            return value;
        }

        if ( key.compare( ENV_CF_GMSA_SECRET_NAME ) == 0 && ecs_variable_name.compare( key ) == 0 )
        {
            return value;
        }

        if ( key.compare( ENV_CF_DOMAIN_CONTROLLER ) == 0 && ecs_variable_name.compare( key ) == 0 )
        {
            return value;
        }
    }

    return "";
}
