#ifndef REDPILL_UART_FIXER_H
#define REDPILL_UART_FIXER_H

typedef struct hw_config hw_config_uart_fixer;
typedef struct uart_runtime_config uart_runtime_config_uart_fixer;
int register_uart_fixer(const hw_config_uart_fixer *hw, const uart_runtime_config_uart_fixer *uart_config);
int unregister_uart_fixer(void);

#endif //REDPILL_UART_FIXER_H
