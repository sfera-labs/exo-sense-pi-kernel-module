#!/usr/bin/python3 -u

# Exo Sense Pi calibration script
#
#     Copyright (C) 2020-2021 Sfera Labs S.r.l.
#
#     For information, visit https://www.sferalabs.cc
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# LICENSE.txt file for more details.

import multiprocessing as mp
import time
import sys
import os

TEMP_STABLE_TIMEOUT = 40 * 60
TEMP_STABLE_MVNG_AVG_ITVL = 3 * 60
TEMP_STABLE_READ_ITVL = 10
TEMP_STABLE_DELTA = 0.1

SYSFS_DIR = '/sys/class/exosensepi/'

TB = None
T2 = None
T1 = None

def read_sysfs_file(file):
	f = open(SYSFS_DIR + file, 'r')
	val = f.read().strip()
	f.close()
	return val

def write_sysfs_file(file, val):
	f = open(SYSFS_DIR + file, 'w')
	f.write(val)
	f.close()

def read_temp(file, idx=-1):
	temp = None
	reads = 5
	attempts = reads * 2
	success = 0
	for _ in range(attempts):
		try:
			val = read_sysfs_file(file)
			if idx >= 0:
				val = val.split()[idx].strip()
			val = int(val) / 100.0
			if temp is None:
				temp = val
			else:
				temp += val
			success += 1
			if success >= reads:
				break
		except:
			pass

	if success < reads:
		raise Exception('Error reading temperature ' + file)
	return temp / success

def read_temps():
	global TB, T2, T1
	TB = read_temp('tha/temp_rh', 1)
	T1 = read_temp('sys_temp/t1')
	T2 = read_temp('sys_temp/t2')

def print_temps():
	global TB, T2, T1
	print('TB: {}'.format(TB))
	print('T1: {}'.format(T1))
	print('T2: {}'.format(T2))

def cpu_use(x):
	print('CPU {} started'.format(x + 1))
	while True:
		pass

def cpus_warmup():
	ps = []
	for x in range(mp.cpu_count()):
		p = mp.Process(target=cpu_use, args=(x,))
		p.start()
		ps.append(p)
	return ps

def wait_for_stable_temps():
	global TB, T2, T1
	avg_n = TEMP_STABLE_MVNG_AVG_ITVL // TEMP_STABLE_READ_ITVL
	TB_buff = []
	T1_buff = []
	T2_buff = []
	for _ in range(TEMP_STABLE_TIMEOUT // TEMP_STABLE_READ_ITVL):
		read_temps()
		# print('- {} ----------'.format(_)) # TODO remove
		# print_temps() # TODO remove
		if len(TB_buff) == avg_n:
			# print('+') # TODO remove
			TB_avg = sum(TB_buff) / avg_n
			T1_avg = sum(T1_buff) / avg_n
			T2_avg = sum(T2_buff) / avg_n
			if abs(TB_avg - TB) < TEMP_STABLE_DELTA and \
				abs(T1_avg - T1) < TEMP_STABLE_DELTA and \
				abs(T2_avg - T2) < TEMP_STABLE_DELTA:
				return True
			TB_buff.pop(0)
			T1_buff.pop(0)
			T2_buff.pop(0)
		TB_buff.append(TB)
		T1_buff.append(T1)
		T2_buff.append(T2)
		for _ in range(TEMP_STABLE_READ_ITVL):
			write_sysfs_file('led/status', 'F')
			time.sleep(1)
	return False

def calibrate():
	global TB, T2, T1

	try:
		os.remove('/etc/modprobe.d/exosensepi.conf')
	except:
		pass

	read_temps()
	print_temps()

	TAMB = TB

	print('Waiting for temperature to stabilize...')
	if not wait_for_stable_temps():
		raise Exception('Process timed out')

	print_temps()

	E1 = TAMB - TB
	DT1 = T1 - T2

	if E1 >= 0:
		raise Exception('No temperature variation: E1={}'.format(E1))

	if DT1 <= 0:
		raise Exception('No internal temperature difference: DT1={}'.format(DT1))

	print('Warming up CPUs...')
	procs = cpus_warmup()

	time.sleep(1)

	print('Waiting for temperature to stabilize...')
	ok = wait_for_stable_temps()

	for p in procs:
		p.terminate()

	for p in procs:
		p.join()

	if not ok:
		raise Exception('Process timed out')

	print_temps()

	E2 = TAMB - TB
	DT2 = T1 - T2

	if E2 >= E1:
		raise Exception('No temperature variation: E1={} E2={}'.format(E1, E2))

	if DT2 <= DT1:
		raise Exception('No internal temperature difference: DT1={} DT2={}'.format(DT1, DT2))

	M = (E2 - E1) / (DT2 - DT1)
	B = E1 - M * DT1

	M = int(M * 1000)
	B = int(B * 1000)

	f = open('/etc/modprobe.d/exosensepi.conf', 'w')
	f.write('options exosensepi temp_calib_m={} temp_calib_b={}'.format(M, B))
	f.close()

	print('Calibration params set: M={} B={}'.format(M, B))

if __name__ == '__main__':
	try:
		print('Exo Sense Pi calibration - v1.0')

		try:
			write_sysfs_file('led/status', '0')
			write_sysfs_file('buzzer/beep', '100')
		except:
			print('\n*** Is the kernel module enabled? ***\n')
			raise

		calibrate()

		os.system("sudo systemctl disable exosensepi-calibrate")
		write_sysfs_file('led/status', '1')
	except:
		try:
			write_sysfs_file('led/status', '0')
		except:
			pass
		raise
	finally:
		write_sysfs_file('buzzer/beep', '100 100 3')
