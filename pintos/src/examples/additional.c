#include <stdio.h>
#include <stdlib.h>
#include <syscall.h>

int
main (int argc, char *argv[])
{
  int values[4];
  int i;

  if (argc != 5)
    {
      printf ("usage: additional <num1> <num2> <num3> <num4>\n");
      return EXIT_FAILURE;
    }

  for (i = 0; i < 4; i++)
    values[i] = atoi (argv[i + 1]);

  printf ("fibonacci(%d) = %d\n", values[0], fibonacci (values[0]));
  printf ("max_of_four_int(%d, %d, %d, %d) = %d\n",
          values[0], values[1], values[2], values[3],
          max_of_four_int (values[0], values[1], values[2], values[3]));
  return EXIT_SUCCESS;
}
