/*******************************************************************************
  * @file       Temperature and Humility Sensor Application Driver     
  * @author 
  * @version
  * @date 
  * @brief
  ******************************************************************************
  * @attention
  *
  *
*******************************************************************************/

/*-------------------------------- Includes ----------------------------------*/
#include "stdint.h"

/*******************************************************************************
 FUNCTION PROTOTYPES
*******************************************************************************/

extern float tem, hum; //定义全剧变量  温度，湿度

int sht30_init(void);
unsigned char SHT3X_CalcCrc(unsigned char *data, unsigned char nbrOfBytes);
unsigned char SHT3X_CheckCrc(unsigned char *pdata, unsigned char nbrOfBytes, unsigned char checksum);
int sht30_get_value(void);
void sht30_SingleShotMeasure(float *temp, float *humi);
/*******************************************************************************
                                      END         
*******************************************************************************/
