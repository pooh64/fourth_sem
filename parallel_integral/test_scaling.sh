for((i = 1; i < 10; i++))
do
	printf "$i;" >>scaling.csv
	TIMEFORMAT='%3R'; (time (./multicore_integrate $i 2>/dev/null)) 2>>scaling.csv
done
