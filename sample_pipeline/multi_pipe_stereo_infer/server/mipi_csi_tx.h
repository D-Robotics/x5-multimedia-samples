#ifndef _MIPI_CSI_TX_H_
#define _MIPI_CSI_TX_H_

int drm_mipi_tx_init(int dma_buf_fd, int width, int height);
void drm_mipi_tx_destroy(void);
int drm_mipi_tx_flush(void);

#endif
