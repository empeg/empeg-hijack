BEGIN	{
		printf("#include <linux/kdb.h>\n");
		printf("#include \"ksym.h\"\n\n");
		printf("#define   KDBMAXSYMTABSIZE 10000\n");
		printf("\n__ksymtab_t __attribute__ ((section(\"kdbsymtab\"))) __kdbsymtab[KDBMAXSYMTABSIZE] = {\n");
		symcount = 0
		printf("/* Generated file */\n") > "ksym.h";
		printf("char __attribute__ ((section(\"kdbstrings\"))) kdb_null[]=\"\";\n") >> "ksym.h";
	}

	{ symname = sprintf("_kdbs%d", symcount);
	  printf("{%s, 0x%s},\n", symname, $1);
	  printf("char __attribute__ ((section(\"kdbstrings\"))) %s[] = \"%s\";\n", symname, $3) >> "ksym.h";
	  symcount = symcount + 1
        }

END	{
		printf(" [%d ... KDBMAXSYMTABSIZE-1] = {kdb_null, 0xf}};\n", symcount);
		printf("int __attribute__ ((section(\"kdbsymtab\"))) __kdbsymtabsize = %d;\n", symcount);
		printf("int __attribute__ ((section(\"kdbsymtab\"))) __kdbmaxsymtabsize = KDBMAXSYMTABSIZE;\n");
	}
