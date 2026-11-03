#include "ad9361/ad9361_api.h"
#include "ad9361/ad9361.h"
#include "xgpio.h"

extern int ad9361_default_init(void);

extern struct ad9361_rf_phy *ad9361_phy;

extern uint32_t TX_ATTEN; 			//Attenuation	Unit:dBm
extern uint32_t TX_LO_FREQ;
extern uint32_t RX_LO_FREQ;
extern uint32_t TX_RF_BANDWIDTH;
extern uint32_t RX_RF_BANDWIDTH;
extern uint32_t TX_SAMPLING_FREQ;
extern uint32_t RX_SAMPLING_FREQ;

int32_t RX_MANUAL_GAIN_DB = 20;

int AD9361_Init()
{
	//Parameter Definition
	int32_t ret;
	uint32_t txFreq;
	uint32_t rxFreq;

	//Parameter Initial
	txFreq = TX_LO_FREQ;
	rxFreq = RX_LO_FREQ;

	//AD9361 Initial
	ad9361_default_init();

	ret = ad9361_set_en_state_machine_mode(ad9361_phy,
									   ENSM_MODE_PINCTRL_FDD_INDEP);
	if (ret < 0) {
		return ret;
	}

	ret = ad9361_spi_write(ad9361_phy->spi, REG_TX_ENABLE_FILTER_CTRL, 0x9F);
	if (ret < 0) {
		return ret;
	}

	ret = ad9361_set_tx_lo_freq(ad9361_phy, txFreq);
	if (ret < 0) {
		return ret;
	}

	ret = ad9361_set_tx_sampling_freq(ad9361_phy, TX_SAMPLING_FREQ);
	if (ret < 0) {
		return ret;
	}

	ret = ad9361_set_tx_rf_bandwidth(ad9361_phy, TX_RF_BANDWIDTH);
	if (ret < 0) {
		return ret;
	}

	ret = ad9361_spi_write(ad9361_phy->spi, REG_RX_ENABLE_FILTER_CTRL, 0x9F);
	if (ret < 0) {
		return ret;
	}

	/* In 1R1T mode the API channel is logical TX1; the driver maps it to
	 * the selected physical TX2 path. */
	ret = ad9361_set_tx_attenuation(ad9361_phy, TX1, TX_ATTEN);
	if (ret < 0) {
		return ret;
	}

	/* A fixed gain is required for repeatable per-bin power and dBm calibration. */
	/* Likewise logical RX1 maps to the configured physical RX2 path. */
	ret = ad9361_set_rx_gain_control_mode(ad9361_phy, RX1, RF_GAIN_MGC);
	if (ret < 0) {
		return ret;
	}

	ret = ad9361_set_rx_lo_freq(ad9361_phy, rxFreq);
	if (ret < 0) {
		return ret;
	}

	ret = ad9361_set_rx_rf_bandwidth(ad9361_phy, RX_RF_BANDWIDTH);
	if (ret < 0) {
		return ret;
	}

	ret = ad9361_set_rx_sampling_freq(ad9361_phy, RX_SAMPLING_FREQ);
	if (ret < 0) {
		return ret;
	}

	ret = ad9361_set_rx_rf_gain(ad9361_phy, RX1, RX_MANUAL_GAIN_DB);
	if (ret < 0) {
		return ret;
	}

	return 0;
}
