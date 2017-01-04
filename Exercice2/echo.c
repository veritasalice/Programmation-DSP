/*
*  Copyright 2003 by Texas Instruments Incorporated.
*  All rights reserved. Property of Texas Instruments Incorporated.
*  Restricted rights to use, duplicate or disclose this code are
*  granted through contract.
*  
*/
/* "@(#) DSP/BIOS 4.90.270 01-08-04 (bios,dsk6713-c04)" */
/*
*  ======== pip_audio.c ========
*
*  This example demonstrates the use of IOM drivers with PIPs using 
*  the PIO adapter with a user defined device mini-driver called
*  "udevCodec". The application performs a loopback.  That is, audio data is
*  read from one PIP connected to an input IOM channel, and the data is 
*  written back out on a PIP connected to an output IOM channel.
*
*  The following objects need to be created in the DSP/BIOS
*  configuration for this application:
*
*  * A UDEV object, which links in a user device driver. In this case
*    the UDEV is a codec based IOM device driver. 
*
*  * A SWI object named swiEcho. Configure the function as _echo,
*    and the mailbox value as 3.
*
*  * 2 PIP objects, one named pipTx, the other pipRx. The length of the
*    buffers should be the same and can be any size. See the comments
*    by the declarations below of pipTx and pipRx for the writer and
*    reader notify function settings.
*
*  * A LOG object named trace, used for status and debug output. Can be
*    any size and can be circular or fixed.
*/

#include <std.h>
#include <math.h>
#include <log.h>
#include <pip.h>
#include <swi.h>
#include <sys.h>

#include <iom.h>
#include <pio.h>

#ifdef _6x_
extern far LOG_Obj trace;
extern far PIP_Obj pipRx; 
extern far PIP_Obj pipTx;
extern far SWI_Obj swiEcho;
#else
extern LOG_Obj trace; 
extern PIP_Obj pipRx;
extern PIP_Obj pipTx;
extern SWI_Obj swiEcho;
#endif

#define LNGBUF 128  // longueur des buffers
/*
*  'pioRx' and 'pioTx' objects will be initialized by PIO_new(). 
*/
PIO_Obj pioRx, pioTx;

float BufIn[LNGBUF+4]; // buffer pour les entrées, 4 cases en plus pour stocker les valeurs précédentes
float BufGraves[LNGBUF+4]; // buffer pour la sortie filtre graves, 4 cases en plus pour stocker les valeurs précédentes
float BufAigus[LNGBUF+4]; // buffer pour la sortie filtre aigus, 4 cases en plus pour stocker les valeurs précédentes
float BufOut[LNGBUF+4]; // buffer pour la sortie filtre médiums, 4 cases en plus pour stocker les valeurs précédentes
float Fe; // Fréquence d'échantillonage
int prev_curseur_graves, prev_curseur_aigus, prev_curseur_mediums; // Valeurs des curseurs à l'instant précédent
float c, d, e, w0_graves; // Constantes filtre fréquences graves
float f, g, h, w0_aigus; // Constantes filtre fréquences aigus
float k, m, n, q, p, w0_mediums, alpha, q1, q2; // Constantes filtre fréquences médiums
int gain_graves = 5;
int gain_aigus = 5;
int gain_mediums = 5;

/*
*  ======== main ========
*
*  Application startup funtion called by DSP/BIOS. Initialize the
*  PIO adapter then return back into DSP/BIOS.
*/
main()
{
    int j;
    Fe = 44100.0;
    alpha = 0.1;
    w0_graves = 2*3.14*400.0/Fe; // Fréquence coupure filtre graves
    w0_aigus = 2*3.14*2500.0/Fe; // Fréquence coupure filtre aigus
    w0_mediums = 2*3.14*1000.0/Fe; // Fréquence coupure filtre aigus
    
    prev_curseur_graves = 0;
    prev_curseur_aigus = 0;
    prev_curseur_mediums = 0;
    
    /*
    * Initialize PIO module
    */
    PIO_init();

    /* Bind the PIPs to the channels using the PIO class drivers */
    PIO_new(&pioRx, &pipRx, "/udevCodec", IOM_INPUT, NULL);
    PIO_new(&pioTx, &pipTx, "/udevCodec", IOM_OUTPUT, NULL);

    /*
    * Prime the transmit side with buffers of silence.
    * The transmitter should be started before the receiver.
    * This results in input-to-output latency being one full
    * buffer period if the pipes is configured for 2 frames.
    */
    PIO_txStart(&pioTx, PIP_getWriterNumFrames(&pipTx), 0);

    /* Prime the receive side with empty buffers to be filled. */
    PIO_rxStart(&pioRx, PIP_getWriterNumFrames(&pipRx));

    LOG_printf(&trace, "pip_audio started");
    
    for (j = 0; j < 4; j++) // initialisation à 0 des 4 premières cases des buffers
    {
        BufIn[j] = 0;
        BufGraves[j] = 0;
        BufAigus[j] = 0;
        BufOut[j] = 0;
    }
}

/*
*  ======== echo ========
*
*  This function is called by the swiEcho DSP/BIOS SWI thread created
*  statically with the DSP/BIOS configuration tool. The PIO adapter
*  posts the swi when an the input PIP has a buffer of data and the
*  output PIP has an empty buffer to put new data into. This function
*  copies from the input PIP to the output PIP. You could easily
*  replace the copy function with a signal processing algorithm.
*/
Void echo(Void)
{
    int i, size;
    short *src, *dst;
    float temp; // variable de stockage temporaire

    /*
    * Check that the precondions are met, that is pipRx has a buffer of
    * data and pipTx has a free buffer.
    */
    if (PIP_getReaderNumFrames(&pipRx) <= 0)
    {
        LOG_error("echo: No reader frame!", 0);
        return;
    }
    if (PIP_getWriterNumFrames(&pipTx) <= 0)
    {
        LOG_error("echo: No writer frame!", 0);
        return;
    }

    /* get the full buffer from the receive PIP */
    PIP_get(&pipRx);
    src = PIP_getReaderAddr(&pipRx);
    size = PIP_getReaderSize(&pipRx) * sizeof(short);

    /* get the empty buffer from the transmit PIP */
    PIP_alloc(&pipTx);
    dst = PIP_getWriterAddr(&pipTx);

    // -----------------------------------------
    // copie l'entrée vers le buffer d'entrée
    // -----------------------------------------
    for (i = 4; i < size + 4; i++)
    {
        BufIn[i] = (float)*src++ / 32768.0; // normalisation du signal entre -1 et +1
    }     

    // -----------------------------------------
    // calcul coefficients des filtres si les curseurs ont été modifiés
    // -----------------------------------------
    if (gain_graves != prev_curseur_graves)
    {
        temp = sqrt(pow(10.0, ((float)gain_graves-5.0)/5.0)); // on calcule la racine carré du gain une seule fois
        c = (2+w0_graves*temp)/(2+w0_graves/temp);
        d = (-2+w0_graves*temp)/(2+w0_graves/temp);
        e = (-2+w0_graves/temp)/(2+w0_graves/temp);
        prev_curseur_graves = gain_graves;
    }
    
    if (gain_aigus != prev_curseur_aigus)
    {	
        temp = sqrt(pow(10.0, ((float)gain_aigus-5.0)/5.0)); // on calcule la racine carré du gain une seule fois
        f = (-2*temp+w0_aigus)/(w0_aigus + 2/temp);
        g = (w0_aigus - 2/temp)/(w0_aigus + 2/temp);
        h = (w0_aigus + 2*temp)/(w0_aigus + 2/temp);
        prev_curseur_aigus = gain_aigus;
    }
    
    if (gain_mediums != prev_curseur_mediums)
    {
        temp = sqrt(pow(10.0, ((float)gain_mediums-5.0)/5.0)); // on calcule la racine carré du gain une seule fois
        q2 = alpha*temp/(1-alpha*alpha);
        q1 = q2/pow(10.0, ((float)gain_mediums-5.0)/5.0);
        k = 4 + w0_mediums*w0_mediums + 2*w0_mediums/q1;
        m = 2*w0_mediums*w0_mediums-8;
        n = 4 + w0_mediums*w0_mediums - 2*w0_mediums/q1;
        q = 4 + w0_mediums*w0_mediums - 2*w0_mediums/q2;
        p = 4 + w0_mediums*w0_mediums + 2*w0_mediums/q2;
        prev_curseur_mediums = gain_mediums;
    }
    
    // ------------------------------------------
    // Filtrage
    // ------------------------------------------
    for (i = 4; i < size + 4; i++)
    {
        BufGraves[i] = BufIn[i]*c + BufIn[i-2]*d - BufGraves[i-2]*e; // calcul de la sortie du filtre pour les graves
        BufAigus[i] = BufGraves[i]*h + BufGraves[i-2]*f - BufAigus[i-2]*g; // calcul de la sortie du filtre pour les aigus
        BufOut[i] = (-m*BufOut[i-2] - q*BufOut[i-4] + k*BufAigus[i] + m*BufAigus[i-2] + n*BufAigus[i-4])/p; // calcul de la sortie du filtre pour les mediums
    }

    //on copie les 4 dernières cases des buffers d'entrée au début de ceux-ci pour la prochaine itération
    for (i = 0; i < 4; i++)
    {
        BufIn[i] = BufIn[size+i];
        BufGraves[i] = BufGraves[size+i];
        BufAigus[i] = BufAigus[size+i];
        BufOut[i] = BufOut[size+i];
    }
    
    // copie le buffer de sortie vers la sortie
    for (i = 4; i < size + 4; i++)
    {
        *dst++ = BufOut[i] *32768.0 ; // reconversion du signal en entier
    }     

    /* Record the amount of actual data being sent */
    PIP_setWriterSize(&pipTx, PIP_getReaderSize(&pipRx));

    /* Free the receive buffer, put the transmit buffer */
    PIP_put(&pipTx);
    PIP_free(&pipRx);
}

