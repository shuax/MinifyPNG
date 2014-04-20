#include <stdio.h>
//#define MINIZ_HEADER_FILE_ONLY
#include "miniz.c"

#define fprintf(...) ((void)0)
#include "zopfli\zlib_container.h"
#include "zopfli\blocksplitter.c"
#include "zopfli\cache.c"
#include "zopfli\hash.c"
#include "zopfli\deflate.c"
#include "zopfli\gzip_container.c"
#include "zopfli\katajainen.c"
#include "zopfli\lz77.c"
#include "zopfli\squeeze.c"
#include "zopfli\tree.c"
#include "zopfli\util.c"
#include "zopfli\zlib_container.c"


Options options;

unsigned long CRC32_MEM(const unsigned char* InStr, unsigned long len)
{
	//生成Crc32的查询表
	unsigned int Crc32Table[256] = {0};
	unsigned int Crc;
	for (unsigned int i = 0; i < 256; i++)
	{
		Crc = i;
		for (unsigned int j = 0; j < 8; j++)
		{
			if (Crc & 1)
				Crc = (Crc >> 1) ^ 0xEDB88320;
			else
				Crc >>= 1;
		}
		Crc32Table[i] = Crc;
	}

	//开始计算CRC32校验值
	Crc = 0xFFFFFFFF;

	//IDAT
	for (unsigned int i = 0; i < 4; i++)
	{
		Crc = (Crc >> 8) ^ Crc32Table[(Crc & 0xFF) ^ "IDAT"[i]];
	}

	for (unsigned int i = 0; i < len; i++)
	{
		Crc = (Crc >> 8) ^ Crc32Table[(Crc & 0xFF) ^ InStr[i]];
	}

	Crc ^= 0xFFFFFFFF;
	return Crc;
}

void MinifyPNG(HWND list, const wchar_t *file, bool SaveBak)
{
    ListBox_AddString(list, file);

    FILE *fp = _wfopen(file, L"rb");
    if(!fp)
    {
        ListBox_AddString(list, L"打开文件失败。");
        ListBox_AddString(list, L"");
        return;
    }
    fseek( fp, 0, SEEK_END);
    int FileLength = ftell(fp);
    fseek( fp, 0, SEEK_SET);

    BYTE *FileBuf = (BYTE*)malloc(FileLength);
    fread(FileBuf,1,FileLength,fp);
    fclose(fp);

    BYTE *ptr = FileBuf;

    BYTE png_sig[] = {0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a};
    if( FileLength<sizeof(png_sig) || memcmp(ptr,png_sig,sizeof(png_sig)) )
    {
        ListBox_AddString(list, L"不是PNG文件。");
        ListBox_AddString(list, L"");
        free(FileBuf);
        return;
    }

    ptr += sizeof(png_sig);

    BYTE *ihdr = 0;
    BYTE *plte = 0;
    BYTE *idat = 0;
    DWORD ihdr_len = 0;
    DWORD plte_len = 0;
    DWORD idat_len = 0;
    while(ptr<FileBuf+FileLength)
    {
        DWORD len = __builtin_bswap32(*(DWORD*)ptr);
        ptr+=4;

        if(memcmp(ptr,"IEND",4)==0)
        {
            break;
        }
        if(memcmp(ptr,"IHDR",4)==0)
        {
            ihdr = ptr;
            ihdr_len = len;
        }
        if(memcmp(ptr,"PLTE",4)==0)
        {
            plte = ptr;
            plte_len = len;
        }
        if(memcmp(ptr,"IDAT",4)==0)
        {
            idat = (BYTE *)realloc(idat, idat_len + len);
            memcpy(idat + idat_len, ptr+4,len);
            idat_len += len;
        }

        ptr+=4; //
        ptr+=len;
        ptr+=4; //CRC32
    }

    if(ihdr && idat)
    {
        DWORD w = __builtin_bswap32(*(DWORD*)(ihdr+4));
        DWORD h = __builtin_bswap32(*(DWORD*)(ihdr+8));

        DWORD out_len = (w*4+1)*h*4;
        BYTE *out_buf = (BYTE *)malloc(out_len);

        if(mz_uncompress(out_buf, &out_len, idat, idat_len)==MZ_OK)
        {
            free(idat);
            //
            ListBox_AddString(list, L"准备重新压缩。");

            unsigned char *zopfli_buf = 0;
            size_t zopfli_size = 0;
            ZlibCompress(&options, out_buf, out_len, &zopfli_buf, &zopfli_size);
            free(out_buf);

            if(SaveBak)
            {
                wchar_t t_file[MAX_PATH];
                wcscpy(t_file, file);

                wchar_t *ext = wcsrchr(file,'\\') + 1;
                t_file[ext-file] = 0;
                wcscat(t_file, L"_");
                wcscat(t_file, ext);

                _wrename(file, t_file);
                //ListBox_AddString(list, L"备份文件完成。");
            }

            FILE *out = _wfopen(file, L"wb");
            if(!out)
            {
                ListBox_AddString(list, L"保存文件失败。");
                ListBox_AddString(list, L"");
                free(FileBuf);
                return;
            }

            //PNG文件头
            fwrite(png_sig,1,sizeof(png_sig),out);

            //PNG IHDR
            fwrite(ihdr-4,1,ihdr_len+12,out);

            //PNG PLTE
            if(plte)
            {
                fwrite(plte-4,1,plte_len+12,out);
            }

            //PNG IDAT
            DWORD zopfli_size_ = __builtin_bswap32(zopfli_size);
            fwrite((void*)&zopfli_size_,1,4,out);
            fwrite((void*)"IDAT",1,4,out);
            fwrite(zopfli_buf,1,zopfli_size,out);

            //IDAT CRC32
            DWORD crc32 = __builtin_bswap32(CRC32_MEM(zopfli_buf,zopfli_size));
            fwrite((void*)&crc32,1,4,out);

            //PNG尾部
            BYTE png_end[] = {0x00,0x00,0x00,0x00,0x49,0x45,0x4e,0x44,0xae,0x42,0x60,0x82};
            fwrite(png_end,1,sizeof(png_end),out);
            DWORD new_len = ftell(out);
            fclose(out);

            free(zopfli_buf);

            wchar_t temp[1024];
            swprintf(temp, L"压缩文件完毕。    文件：%d 字节 -> %d 字节    压缩率：%.2f%%", FileLength, new_len, 100.0*new_len/FileLength);
            ListBox_AddString(list, temp);

            ListBox_AddString(list, L"");
            free(FileBuf);
            return;
        }
        else
        {
            free(idat);
            free(out_buf);
            ListBox_AddString(list, L"异常的PNG文件。");
            ListBox_AddString(list, L"");
            free(FileBuf);
            return;
        }


    }
    else
    {
        ListBox_AddString(list, L"不是有效的PNG文件。");
        ListBox_AddString(list, L"");
        free(FileBuf);
        return;
    }

    return;
}
