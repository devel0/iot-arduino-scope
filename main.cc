#include <stdio.h>
#include <stdlib.h>
#include <GL/glut.h>
#include <GL/freeglut_ext.h>
#include <fcntl.h>
#include <unistd.h>
#include <memory.h>
#include <termios.h>

#include <fstream>
#include <chrono>
#include <list>
#include <deque>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <iostream>
#include <algorithm>

#include "AdcEval.h"
#include "Global.h"
#include "SignalStat.h"
#include "Global.h"

using namespace std;

thread thReadSerial;
auto start = std::chrono::system_clock::now();
deque<unsigned long> lst;
const int lstMaxSize = 800;
mutex lstMutex;
bool started = false;
const int ADCVALUEMAX = 1023;
const int windowHeightMargin = 20;
double vFactor = 1;
double hFactor = 1;
bool pointMode = false;
bool textInfo = true;
int deltaV = 0;
string serialPortName;
const int BUFSIZE = 64; //3*1024;
uint8_t buf[BUFSIZE];

SignalStat signalStat;

void reshape(int w, int h)
{
  glViewport(0, 0, w, h);
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glOrtho(0, w, 0, h, -1, 1);
  glScalef(1, 1, 1);
  glTranslatef(0, 0, 0);
}

void drawString(int x, int y, char *s)
{
  glRasterPos2i(x, y);
  while (*s)
    glutBitmapCharacter(GLUT_BITMAP_HELVETICA_12, *(s++));
}

auto lastDisplay = chrono::system_clock::now();
double lastVmin = 0;
double lastVmax = 0;
double lastVppk = 0;
double lastFreq = 0;

void display()
{
  if (!started)
    started = true;

  glClear(GL_COLOR_BUFFER_BIT);
  glColor3f(1, 1, 1);

  auto width = glutGet(GLUT_WINDOW_WIDTH);
  auto height = glutGet(GLUT_WINDOW_HEIGHT);
  auto global = Global::Instance();

  if (textInfo)
  {
    char str[1024];

    auto lineH = 15;

    auto cur = chrono::system_clock::now();
    auto diff = chrono::duration_cast<chrono::milliseconds>(cur - lastDisplay);
    if (diff.count() > 150)
    {
      lastDisplay = cur;
      lastVmin = signalStat.GetVmin();
      lastVmax = signalStat.GetVmax();
      lastVppk = signalStat.GetVppk();
      lastFreq = signalStat.GetFreq();
    }

    sprintf(str, "Vmin = %.2f", lastVmin);
    drawString(width - 100, height - windowHeightMargin, str);

    sprintf(str, "Vmax = %.2f", lastVmax);
    drawString(width - 100, height - windowHeightMargin - 1 * lineH, str);

    sprintf(str, "Vppk = %.2f", lastVppk);
    drawString(width - 100, height - windowHeightMargin - 2 * lineH, str);

    if (lastFreq < 1e3)
      sprintf(str, "Freq = %.0f  Hz", lastFreq);
    else if (lastFreq >= 1e3 && lastFreq < 1e6)
      sprintf(str, "Freq = %.0f kHz", lastFreq / 1e3);
    else if (lastFreq >= 1e6)
      sprintf(str, "Freq = %.0f Mhz", lastFreq / 1e6);

    drawString(width - 100, height - windowHeightMargin - 3 * lineH, str);
  }

  {
    glBegin(pointMode ? GL_POINTS : GL_LINES);

    auto heightMid = height / 2;

    auto adcValueMid = ADCVALUEMAX / 2.0;

    lstMutex.lock();
    int w = min((int)lst.size(), width);
    for (int i = 0; i < w; ++i)
    {
      if (pointMode || i < w)
      {
        double y = windowHeightMargin + heightMid +
                   (lst[i] - adcValueMid + deltaV) / adcValueMid * (heightMid - 2 * windowHeightMargin) * vFactor;
        glVertex2i(i * hFactor, y);
      }

      if (!pointMode)
      {
        double y = windowHeightMargin + heightMid +
                   (lst[i + 1] - adcValueMid + deltaV) / adcValueMid * (heightMid - 2 * windowHeightMargin) * vFactor;
        glVertex2i((i + 1) * hFactor, y);
      }
    }
    lstMutex.unlock();
  }

  glColor3f(1, 1, 0);

  // min,max
  {
    glBegin(GL_LINES);

    auto heightMid = height / 2;

    auto adcValueMid = ADCVALUEMAX / 2.0;

    int xmin = 0;
    int xmax = width;

    double voltages[] = {
        signalStat.GetVmin(),
        signalStat.GetVmax(),
        signalStat.GetVminThresHold(),
        signalStat.GetVmaxThresHold()};

    for (int i = 0; i < sizeof(voltages) / sizeof(double); ++i)
    {
      auto adcv = global.adcEval.GetADC(voltages[i]);

      double y = windowHeightMargin + heightMid +
                 (adcv - adcValueMid + deltaV) / adcValueMid * (heightMid - 2 * windowHeightMargin) * vFactor;
      glVertex2i(xmin, y);
      glVertex2i(xmax, y);
    }
  }

  glEnd();

  glFinish();
  //glFlush();
  glutSwapBuffers();
}

void idleFunc()
{
  auto cur = chrono::system_clock::now();
  auto diff = chrono::duration_cast<chrono::milliseconds>(cur - start);
  if (diff.count() > 20) // 50Hz
  {
    start = cur;

    if (started)
    {
      glutPostRedisplay();
    }

    start = cur;
  }
}

void processADC(int adc)
{
  signalStat.AddAdcValue(adc);

  lstMutex.lock();
  if (lst.size() > lstMaxSize)
    lst.pop_front();
  lst.push_back(adc);
  lstMutex.unlock();
}

void thReadSerialFn()
{
  int USB = open(serialPortName.c_str(), O_RDWR | O_NOCTTY);

  {
    struct termios tty;
    memset(&tty, 0, sizeof tty);

    cfsetispeed(&tty, (speed_t)115200);

    tty.c_cflag &= ~PARENB;
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;

    tty.c_cflag &= ~CRTSCTS;
    tty.c_cc[VMIN] = 1;
    tty.c_cc[VTIME] = 5;
    tty.c_cflag |= CREAD | CLOCAL;

    cfmakeraw(&tty);

    tcflush(USB, TCIFLUSH);
    tcsetattr(USB, TCSANOW, &tty);
  }

  while (true)
  {
    int n = read(USB, buf, BUFSIZE);
    int i = 0;

    // seek start
    while (i < n && buf[i] != '\r' && buf[i] != '\n')
      ++i;

    while (i < n)
    {
      // skip newline
      while (i < n && (buf[i] == '\0' || buf[i] == '\r' || buf[i] == '\n'))
        ++i;

      int value = 0;

      int j = i;
      // seek newline
      while (j < n && buf[j] != '\r' && buf[j] != '\n')
        ++j;

      // check incomplete last element
      if (buf[j] != '\r' && buf[j] != '\n')
      {
        break;
      }

      // replace newline with null char
      buf[j] = '\0';

      // convert value
      value = atoi((const char *)buf + i);
      processADC(value);

      i = j;

      // debug to terminal
      //cout << value << endl;
    }

    //signalStat.Reset();
  }
}

void keyboard(unsigned char c, int x, int y)
{
  switch (c)
  {
  case ' ':
    started = !started;
    break;
  case 'v':
    vFactor /= 1.2;
    cout << "vFactor = " << vFactor << endl;
    break;
  case 'V':
    vFactor *= 1.2;
    cout << "vFactor = " << vFactor << endl;
    break;
  case 'h':
    hFactor /= 1.2;
    cout << "hFactor = " << hFactor << endl;
    break;
  case 'H':
    hFactor *= 1.2;
    cout << "hFactor = " << hFactor << endl;
    break;
  case 'p':
    pointMode = !pointMode;
    break;
  case 't':
    textInfo = !textInfo;
    break;
  }
}

void keyboardSpecial(int key, int x, int y)
{
  switch (key)
  {
  case GLUT_KEY_UP:
    deltaV += 50 / vFactor;
    break;
  case GLUT_KEY_DOWN:
    deltaV -= 50 / vFactor;
    break;
  }
}

int main(int argc, char **argv)
{
  if (argc != 2)
  {
    cout << "Syntax: " << argv[0] << " <serial-port>" << endl;

    return 1;
  }

  serialPortName = argv[1];

  glutInit(&argc, argv);
  glutCreateWindow("arduinoscope");
  glutDisplayFunc(display);
  glutReshapeFunc(reshape);
  glutKeyboardFunc(keyboard);
  glutSpecialFunc(keyboardSpecial);
  glutIdleFunc(idleFunc);
  glutReshapeWindow(800, 600);

  thReadSerial = thread(thReadSerialFn);

  glutMainLoop();

  return 0;
}