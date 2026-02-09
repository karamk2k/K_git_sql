#include <stdio.h>
#include <mysql/mysql.h>
#include "mysql_service.h"

int main() {
    DBConfig config = {0};

    if (load_config(&config) != 0) {
        printf("Failed to load config\n");
        return 1;
    }
    
    printf("DB Host: %s\n", config.host);
    printf("DB User: %s\n", config.user);
    printf("DB Port: %d\n", config.port);
    
    MYSQL *conn = connect_db(&config);
    if (!conn) {
        free_config(&config);
        return 1;
    }
    test_connection(conn);
    close_connection(conn);

    // Watch for changes (infinite loop)
    watch_database(&config);

    free_config(&config);
    return 0;
}