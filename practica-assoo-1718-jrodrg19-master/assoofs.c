#include <linux/module.h>       /* Needed by all modules */
#include <linux/sched.h> 
#include <linux/kernel.h>       /* Needed for KERN_INFO  */
#include <linux/init.h>         /* Needed for the macros */
#include <linux/fs.h>           /* libfs stuff           */
#include <asm/uaccess.h>        /* copy_to_user          */
#include <linux/buffer_head.h>  /* buffer_head           */
#include <linux/slab.h>         /* kmem_cache            */
#include "assoofs.h"		    /* Our own library	 */

#include <linux/namei.h>
#include <linux/random.h>
#include <linux/version.h>
#include <linux/jbd2.h>
#include <linux/parser.h>
#include <linux/blkdev.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Javier Rodriguez Gonzalez");

struct assoofs_inode_info *assoofs_get_inode(struct super_block *sb, uint64_t inode_no);
static struct inode *assoofs_iget(struct super_block *sb, int ino);
struct dentry *assoofs_lookup(struct inode *parent_inode, struct dentry *child_dentry, unsigned int flags);
static int assoofs_mkdir(struct inode *dir, struct dentry *dentry, umode_t mode);
int assoofs_fill_super(struct super_block *sb, void *data, int silent);
static struct dentry *assoofs_mount(struct file_system_type *fs_type, int flags, const char *dev_name, void *data);
ssize_t assoofs_read(struct file *filp, char __user *buf, size_t len, loff_t *ppos);
ssize_t assoofs_write(struct file *filp, const char __user *buf, size_t len, loff_t *ppos);
static int __init assoofs_init(void);
static void __exit assoofs_exit(void);
int assoofs_inode_save(struct super_block *sb, struct assoofs_inode_info *assoofs_inode);
struct assoofs_inode_info *assoofs_inode_search(struct super_block *sb, struct assoofs_inode_info *start, struct assoofs_inode_info *buscar);
void assoofs_sb_sync(struct super_block *vsb);
void assoofs_inode_add(struct super_block *vsb, struct assoofs_inode_info *inode);
int assoofs_sb_get_a_freeblock(struct super_block *vsb, uint64_t * out);
static int assoofs_sb_get_objects_count(struct super_block *vsb, uint64_t * out);
static int assoofs_iterate(struct file *filp, struct dir_context *ctx);
static int assoofs_create_fs_object(struct inode *dir, struct dentry *dentry, umode_t mode);
void assoofs_destroy_inode(struct inode *inode);

/* Nos devolverá la información que nos interesa, con el formato de nuestra struct */
static inline struct assoofs_super_block_info *ASSOOFS_SB(struct super_block *sb)
{
	return sb -> s_fs_info;
}

/* Nos devolverá la información que nos interesa, con el formato de nuestra struct */
static inline struct assoofs_inode_info *ASSOOFS_INODE(struct inode *inode)
{
	return inode -> i_private;
}

// (!) ¿Se llama?
void assoofs_sb_sync(struct super_block *vsb)
{
	struct buffer_head *bh;
	struct assoofs_super_block_info *sb = ASSOOFS_SB(vsb);
    printk(KERN_INFO "assoofs_sb_sync called");

	bh = sb_bread(vsb, ASSOOFS_SUPERBLOCK_BLOCK_NUMBER);
	BUG_ON(!bh);

	bh -> b_data = (char *) sb;
	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);
}

/* La estructura file_system_type está determinada en la biblioteca
 * linux/fs.h
 *
 * Nosotros tenemos que definir una estructura particular para el módulo.
 */

/* Función que asigna operaciones al super_bloque */
static const struct super_operations assoofs_sops = {
    .destroy_inode = assoofs_destroy_inode,
};

/* Función que creará un fichero */
static int assoofs_create(struct inode *dir, struct dentry *dentry, umode_t mode, bool excl) {
    printk(KERN_INFO "[assoofs_create] > Call starts. Calling [assoofs_create_fs_object]...");
    return assoofs_create_fs_object(dir, dentry, mode);
}

static struct kmem_cache *assoofs_inode_cachep;

/* Función que devolverá el inodo, si existe, correspondiente al número introducido */
struct assoofs_inode_info *assoofs_get_inode(struct super_block *sb,
					  uint64_t inode_no)
{

    /* ASSOOFS_SB(superbloque) es lo mismo que devolver el campo s_fs_info del superbloque;
     es decir, el sb -> s_fs_info (información sobre el sistema de ficheros) */
	struct assoofs_super_block_info *assoofs_sb = ASSOOFS_SB(sb);
    struct assoofs_inode_info *assoofs_inode = NULL;
    struct assoofs_inode_info *inode_buffer = NULL;

    printk(KERN_INFO "[assoofs_get_inode] > Call starts. Trying to obtain inode no. %lld.", inode_no);

	int i;
	struct buffer_head *bh;

	/* SUPERBLOCK Block Read: Lee el bloque que se le pasa por parámetro */
	bh = sb_bread(sb, ASSOOFS_INODESTORE_BLOCK_NUMBER);

	assoofs_inode = (struct assoofs_inode_info *) bh -> b_data;

	for (i = 0; i < assoofs_sb -> inodes_count; i++) {
		if (assoofs_inode -> inode_no == inode_no) {
            inode_buffer = kmem_cache_alloc(assoofs_inode_cachep, GFP_KERNEL);
			memcpy(inode_buffer, assoofs_inode, sizeof(*inode_buffer));
			break;
		}
		assoofs_inode++;
	}

    printk(KERN_INFO "[assoofs_get_inode] > Call finished. Inode %lld obtained.", inode_buffer -> inode_no);

	brelse(bh);
	return inode_buffer;
}

/* Función que asigna operaciones para manejar inodos */
static struct inode_operations assoofs_inode_ops = {
    .create = assoofs_create,
    .lookup = assoofs_lookup,
    .mkdir = assoofs_mkdir,
};

/* Función que creará un directorio */
static int assoofs_mkdir(struct inode *dir, struct dentry *dentry, umode_t mode) {
    return assoofs_create_fs_object(dir, dentry, S_IFDIR | mode);
}


/* ========================================================== */
/* IMPLEMENTACIÓN de funciones que sirven para manejar inodos */
/* ========================INICIO============================ */

/* Estructura para manejar archivos */
const struct file_operations assoofs_file_operations = {
    .read = assoofs_read,
    .write = assoofs_write,
};

/* FUNCIONES PARA ARCHIVOS */
/* Función leer, para archivos */
ssize_t assoofs_read(struct file *filp, char __user *buf, size_t len, loff_t *ppos) {
    struct assoofs_inode_info *inode = ASSOOFS_INODE(filp -> f_path.dentry -> d_inode);
    struct buffer_head *bh;

    char *buffer;
    int nbytes;

    printk(KERN_INFO "[assoofs_read] > Call starts. Trying to read %lu inode in [%llu] datablock.", filp -> f_path.dentry -> d_inode -> i_ino, inode -> data_block_number);

    if (*ppos >= inode -> file_size) {
        //printk(KERN_ERR "[assoofs_read] > Trying to access out of inode's memory position (ppos: %d - file_size: %d).", *ppos, inode -> file_size);
        return 0;
    }

    bh = sb_bread(filp -> f_path.dentry -> d_inode -> i_sb, inode -> data_block_number);

    if (!bh) {
        printk(KERN_ERR "[assoofs_read] > Reading the block number [%llu] failed.", inode -> data_block_number);
        return 0;
    }

    buffer = (char *) bh -> b_data;
    nbytes = min((size_t) inode -> file_size, len);

    /* Copiamos a la memoria del usuario, esto es, la información guardada en el bloque la traemos a un nivel más externo. */
    if (copy_to_user(buf, buffer, nbytes)) {
        brelse(bh);
        printk(KERN_ERR "[assoofs_read] > Error copying file contetns to the userspace buffer \n");
        return -EFAULT;
    }

    printk(KERN_INFO "[assoofs_read] > Call finished. Inode no. %lu read in [%llu] datablock.", filp -> f_path.dentry -> d_inode -> i_ino, inode -> data_block_number);

    brelse(bh);
    *ppos += nbytes;

    return nbytes;
}

/* Función escribir, para archivos */
ssize_t assoofs_write(struct file *filp, const char __user *buf, size_t len, loff_t *ppos) {
    struct inode *inode;
    struct assoofs_inode_info *assoofs_inode;
    struct buffer_head *bh;
    struct super_block *sb;
    struct assoofs_super_block_info *assoofs_sb;
    handle_t *handle;

    printk(KERN_INFO "[assoofs_write] > Call starts. Trying to write (%lu) bytes in %lu inode.", len, filp -> f_path.dentry -> d_inode -> i_ino);

    char *buffer;

    int retval;

    sb = filp -> f_path.dentry -> d_inode -> i_sb;
    assoofs_sb = ASSOOFS_SB(sb);

    if (retval)
        return retval;

    inode = filp -> f_path.dentry -> d_inode;
    assoofs_inode = ASSOOFS_INODE(inode);

    bh = sb_bread(filp -> f_path.dentry -> d_inode -> i_sb, assoofs_inode -> data_block_number);

    if (!bh) {
        printk(KERN_ERR "[assoofs_write] > Reading the block number [%llu] failed.", assoofs_inode -> data_block_number);
        return 0;
    }

    buffer = (char *) bh -> b_data;
    printk(KERN_INFO "[assoofs_write] > Valor de buffer: %s", buffer);
    buffer += *ppos;

    /* Copiaremos desde la parte externa (usuario) la información facilitada hasta nivel de kernel. */
    if (copy_from_user(buffer, buf, len)) {
        brelse(bh);
        printk(KERN_ERR "[assoofs_write] > Error copying file contents from the userspace buffer to the kernel space.\n");
        return -EFAULT;
    }
    *ppos += len;

    brelse(bh);

    assoofs_inode -> file_size = *ppos;
    printk(KERN_INFO "[assoofs_write] > Trying to save inode no. %lu.", assoofs_inode -> inode_no);
    retval = assoofs_inode_save(sb, assoofs_inode);
    printk(KERN_INFO "[assoofs_write] > Inode no. %lu saved.", assoofs_inode -> inode_no);

    if (retval) {
        len = retval;
    }

    printk(KERN_INFO "[assoofs_write] > Call finished. Trying to write (%lu) bytes in %lu inode.", len, filp -> f_path.dentry -> d_inode -> i_ino);
    
    return len;
}

struct assoofs_inode_info *assoofs_inode_search(struct super_block *sb, struct assoofs_inode_info *start, struct assoofs_inode_info *buscar) {
	uint64_t count = 0;
	while (start -> inode_no != buscar -> inode_no && count < ASSOOFS_SB(sb) -> inodes_count) {
		count++;
		start++;
	}

	if (start -> inode_no == buscar -> inode_no) {
		return start;
	}

	return NULL;
}

void assoofs_inode_add(struct super_block *vsb, struct assoofs_inode_info *inode) {
	struct assoofs_super_block_info *sb = ASSOOFS_SB(vsb);
	struct buffer_head *bh;
	struct assoofs_inode_info *inode_iterator;

    printk(KERN_INFO "[assoofs_inode_add] > Call starts. Trying to add a new inode of %lu bytes.", inode -> file_size);

    /* Leemos del bloque del superbloque donde se almacenan los inodos */
	bh = sb_bread(vsb, ASSOOFS_INODESTORE_BLOCK_NUMBER);

	inode_iterator = (struct assoofs_inode_info *) bh -> b_data;

	/* Añadimos un nuevo inodo al final de nuestra estructura de inodos del super bloque. */
	inode_iterator += sb -> inodes_count;

    /* Copiamos los bytes del inodo a este nuevo inodo situado al final del bloque (nuevo inodo). */
	memcpy(inode_iterator, inode, sizeof(struct assoofs_inode_info));
    printk(KERN_INFO "[assoofs_inode_add] > Inode no. %lu added.", sb -> inodes_count);
	sb -> inodes_count++;

	mark_buffer_dirty(bh);
	assoofs_sb_sync(vsb);
	brelse(bh);
    printk(KERN_INFO "[assoofs_inode_add] > Call finished. There are now %lu inodes in superblock-inodestore.", sb -> inodes_count);
}


/* Almacena el inodo modificado */
int assoofs_inode_save(struct super_block *sb, struct assoofs_inode_info *assoofs_inode) {
	struct assoofs_inode_info *inode_iterator;
	struct buffer_head *bh;

    printk(KERN_INFO "[assoofs_inode_save] > Call starts. Trying to store the inode no. %lu", assoofs_inode -> inode_no);

	bh = sb_bread(sb, ASSOOFS_INODESTORE_BLOCK_NUMBER);
	BUG_ON(!bh);

	inode_iterator = assoofs_inode_search(sb, (struct assoofs_inode_info *)bh -> b_data, assoofs_inode);

	if (likely(inode_iterator)) {
		memcpy(inode_iterator, assoofs_inode, sizeof(*inode_iterator));
		printk(KERN_INFO "[assoofs_inode_save] > The inode %lu has been updated and stored in memory.", inode_iterator -> inode_no);
		mark_buffer_dirty(bh);
		sync_dirty_buffer(bh);
        printk(KERN_INFO "[assoofs_inode_save] > Call finished. The inode no. %lu has been stored successfully.", inode_iterator -> inode_no);
	} else {
		printk(KERN_ERR "[assoofs_inode_save] > The new filesize [%lu] could not be stored to the inode.", sizeof(*inode_iterator));
		return -EIO;
	}

	brelse(bh);

	return 0;
}

int assoofs_sb_get_a_freeblock(struct super_block *vsb, uint64_t * out)
{
	struct assoofs_super_block_info *sb = ASSOOFS_SB(vsb);
	int i;
	int ret = 0;

    printk(KERN_INFO "[assoofs_sb_get_a_free_block] > Call started. Trying to find a free block. There are %lu blocks in use.", ASSOOFS_SB(vsb) -> inodes_count);

	/* Buscaremos un inodo vacío (a partir del 3 son para datos) para almacenar. */
	for (i = 3; i < ASSOOFS_MAX_FILESYSTEM_OBJECTS_SUPPORTED; i++) {
		if (sb -> free_blocks & (1 << i)) {
            printk(KERN_INFO "[assoofs_sb_get_a_free_block] > The block no. %lu is free.", i);
			break;
		}
	}

	if (unlikely(i == ASSOOFS_MAX_FILESYSTEM_OBJECTS_SUPPORTED)) {
		printk(KERN_ERR "[assoofs_sb_get_a_free_block] > There are no more blocks available.");
		ret = -ENOSPC;
		goto end;
	}

	*out = i;

	/* Eliminamos el bloque de la lista de bloques disponibles */
	sb -> free_blocks &= ~(1 << i);

	assoofs_sb_sync(vsb);

end:
    printk(KERN_INFO "[assoofs_sb_get_a_free_block] > Call finished. Found %lu block free.", i);
	return ret;
}

static int assoofs_sb_get_objects_count(struct super_block *vsb, uint64_t *out)
{
	struct assoofs_super_block_info *sb = ASSOOFS_SB(vsb);

    printk(KERN_INFO "[assoofs_sb_get_objects_count] > Call starts. Trying to count the number of objects in super block.");

	*out = sb -> inodes_count;

    printk(KERN_INFO "[assoofs_sb_get_objects_count] > Call finished. There are %lu objects (this value has been copied to 2nd argument passed through function).", sb -> inodes_count);

	return 0;
}

/* Estructura para manejar directorios */
const struct file_operations assoofs_dir_operations = {
    .owner = THIS_MODULE,
    .iterate = assoofs_iterate,
};

/* FUNCIONES PARA DIRECTORIOS */
/* Función iterar, para directorios */
static int assoofs_iterate(struct file *filp, struct dir_context *ctx) {
	loff_t pos;
	struct inode *inode;
	struct super_block *sb;
	struct buffer_head *bh;
	struct assoofs_inode_info *assoofs_inode;
	struct assoofs_dir_record_entry *record;
	int i;

    

    if (ctx -> pos)
        return 0;

    /* (!) filp -> f_dentry ha sido sustituido, se debe acceder como filp -> f_path.dentry (mirar bibliografía)*/
    pos = ctx -> pos;
	inode = filp -> f_path.dentry -> d_inode;
	sb = inode -> i_sb;

    printk(KERN_INFO "[assoofs_iterate] > Call starts. This DIR? (%s) is in inode no. %lu.", filp -> f_path.dentry -> d_name.name, inode -> i_ino);

	if (pos) {
		/* FIXME: We use a hack of reading pos to figure if we have filled in all data.
		 * We should probably fix this to work in a cursor based model and
		 * use the tokens correctly to not fill too many data in each cursor based call */
		return 0;
	}

	assoofs_inode = ASSOOFS_INODE(inode);

	if (unlikely(!S_ISDIR(assoofs_inode -> mode))) {
		printk(KERN_ERR "[assoofs_iterate] > The inode (ASSOOFS_INODE: [%llu] - INODE: [%lu]) for object [%s] is not a directory.", assoofs_inode -> inode_no, inode -> i_ino, filp -> f_path.dentry -> d_name.name);
		return -ENOTDIR;
	}

	bh = sb_bread(sb, assoofs_inode -> data_block_number);
	BUG_ON(!bh);

	record = (struct assoofs_dir_record_entry *) bh -> b_data;
	for (i = 0; i < assoofs_inode -> dir_children_count; i++) {
		dir_emit(ctx, record -> filename, ASSOOFS_FILENAME_MAXLEN, record -> inode_no, DT_UNKNOWN);
		ctx -> pos += sizeof(struct assoofs_dir_record_entry);
		pos += sizeof(struct assoofs_dir_record_entry);
		record++;
        printk(KERN_INFO "[assoofs_iterate] > Moving through %s", record -> filename);
	}
	brelse(bh);

    printk(KERN_INFO "[assoofs_iterate] > Call finished. Iteration OK!");

	return 0;
}



/* ========================================================== */
/* IMPLEMENTACIÓN de funciones que sirven para manejar inodos */
/* ===========================FIN============================ */


static struct inode *assoofs_iget(struct super_block *sb, int ino) {
	struct inode *inode;
	struct assoofs_inode_info *assoofs_inode;

    printk(KERN_INFO "[assoofs_iget] > Call stats. Trying to get the inode no. %lu.", ino);

	assoofs_inode = assoofs_get_inode(sb, ino);

	inode = new_inode(sb);
	inode -> i_ino = ino;
	inode -> i_sb = sb;
	inode -> i_op = &assoofs_inode_ops;

	if (S_ISDIR(assoofs_inode -> mode)) {
		inode -> i_fop = &assoofs_dir_operations;
        printk(KERN_INFO "[assoofs_iget] > The inode no. %lu is a DIR.", ino);
	} else if (S_ISREG(assoofs_inode -> mode)) {
		inode -> i_fop = &assoofs_file_operations;
        printk(KERN_INFO "[assoofs_iget] > The inode no. %lu is a FILE.", ino);
	} else
		printk(KERN_ERR "[assoofs_iget] > Unknown inode type. Neither a DIR nor a FILE.");

	/* Deberíamos actualizar la información de acceso al fichero o directorio */
	inode->i_atime=inode->i_mtime = inode->i_ctime = current_time(inode);
	inode -> i_private = assoofs_inode;

    printk(KERN_INFO "[assoofs_iget] > Call finished. We have the inode no. %lu.", ino);

	return inode; 
}

/* IMPLEMENTACIÓN de funciones que sirven para manejar inodos */
struct dentry *assoofs_lookup(struct inode *parent_inode, struct dentry *child_dentry, unsigned int flags) {
	printk(KERN_INFO "[assoofs_lookup] > Call starts. It will look for %s FILE/DIR.", child_dentry -> d_name.name);

	struct assoofs_inode_info *parent = ASSOOFS_INODE(parent_inode);
	struct super_block *sb = parent_inode -> i_sb;
	struct buffer_head *bh;
	struct assoofs_dir_record_entry *record;
	int i;

	bh = sb_bread(sb, parent -> data_block_number);
    BUG_ON(!bh);

	record = (struct assoofs_dir_record_entry *) bh -> b_data;
    printk(KERN_INFO "[assoofs_lookup] > Looking for %s FILE/DIR.", child_dentry -> d_name.name);
	for (i = 0; i < parent -> dir_children_count; i++) {
		if (!strcmp(record -> filename, child_dentry -> d_name.name)) {
		  
		    struct inode *inode = assoofs_iget(sb, record -> inode_no);
		    //inode_init_owner(inode, parent_inode, ((struct assoofs_inode_info *) inode -> i_private) -> mode);
            inode_init_owner(inode, parent_inode, ASSOOFS_INODE(inode) -> mode);
		    d_add(child_dentry, inode);
            printk(KERN_INFO "[assoofs_lookup] > Found %s.", record -> filename);
			return child_dentry;
            // (!) Previously: return NULL.
		}
		record++;
	}

    printk(KERN_INFO "[assoofs_lookup] > Call finished. File %s not found.", child_dentry -> d_name.name);

	return NULL;
}

static int assoofs_create_fs_object(struct inode *dir, struct dentry *dentry, umode_t mode) {
	struct inode *inode;
	struct assoofs_inode_info *assoofs_inode;
	struct super_block *sb;
	struct assoofs_inode_info *parent_dir_inode;
	struct buffer_head *bh;
	struct assoofs_dir_record_entry *dir_contents_datablock;
	uint64_t count;
	int ret;
    printk(KERN_INFO "[assoofs_create_fs_object] > Call starts. Trying to create %s FILE/DIR.", dentry -> d_name.name);

	sb = dir -> i_sb;
	ret = assoofs_sb_get_objects_count(sb, &count);

	if (unlikely(count >= ASSOOFS_MAX_FILESYSTEM_OBJECTS_SUPPORTED)) {
		printk(KERN_ERR "[assoofs_create_fs_object] > Maximum number of objects [%lu] supported by ASSOOFS is already reached.", ASSOOFS_MAX_FILESYSTEM_OBJECTS_SUPPORTED);
		return -ENOSPC;
	}

	if (!S_ISDIR(mode) && !S_ISREG(mode)) {
		printk(KERN_ERR "[assoofs_create_fs_object] > Creation request but for !FILE/DIR.");
		return -EINVAL;
	}

	inode = new_inode(sb);
	if (!inode)
		return -ENOMEM;

	inode -> i_sb = sb;
	inode -> i_op = &assoofs_inode_ops;
	inode->i_atime=inode->i_mtime = inode->i_ctime = current_time(inode);
	inode -> i_ino = (count + ASSOOFS_START_INO - ASSOOFS_RESERVED_INODES + 1);

	assoofs_inode = kmem_cache_alloc(assoofs_inode_cachep, GFP_KERNEL);
	assoofs_inode -> inode_no = inode -> i_ino;
	inode -> i_private = assoofs_inode;
	assoofs_inode -> mode = mode;

	if (S_ISDIR(mode)) {
		printk(KERN_INFO "[assoofs_create_fs_object] > New DIR [%s] creation request. It will be stored in inode no. %lu.", dentry -> d_name.name, inode -> i_ino);
		assoofs_inode -> dir_children_count = 0;
		inode -> i_fop = &assoofs_dir_operations;
	} else if (S_ISREG(mode)) {
		printk(KERN_INFO "[assoofs_create_fs_object] > New FILE [%s] creation request. It will be stored in inode no. %lu.", dentry -> d_name.name, inode -> i_ino);
		assoofs_inode -> file_size = 0;
		inode -> i_fop = &assoofs_file_operations;
	}

	/* Obtenemos un bloque libre */
	ret = assoofs_sb_get_a_freeblock(sb, &assoofs_inode -> data_block_number);
	if (ret < 0) {
		printk(KERN_ERR "[assoofs_create_fs_object] > There are no blocks available.");
		return ret;
	}

	assoofs_inode_add(sb, assoofs_inode);

	parent_dir_inode = ASSOOFS_INODE(dir);
	bh = sb_bread(sb, parent_dir_inode -> data_block_number);
	BUG_ON(!bh);

	dir_contents_datablock = (struct assoofs_dir_record_entry *) bh -> b_data;

	/* Navegar al último directorio */
	dir_contents_datablock += parent_dir_inode -> dir_children_count;
	dir_contents_datablock -> inode_no = assoofs_inode -> inode_no;
	strcpy(dir_contents_datablock -> filename, dentry-> d_name.name);

	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);

	parent_dir_inode -> dir_children_count++;
	ret = assoofs_inode_save(sb, parent_dir_inode);
	if (ret) {
        printk(KERN_ERR "[assoofs_create_fs_object] > Inode cannot be saved. We need to undo the things we did on this call.");
		// Deshacer lo hecho anteriormente si no se hubiese podido guardar por alguna razón
		return ret;
	}

	inode_init_owner(inode, dir, mode);
	d_add(dentry, inode);

    printk(KERN_INFO "[assoofs_create_fs_object] > Call finished. FILE/DIR %s stored and saved.", dentry -> d_name.name);

	return 0;
}

void assoofs_destroy_inode(struct inode *inode) {
	struct assoofs_inode_info *assoofs_inode = ASSOOFS_INODE(inode);

	printk(KERN_INFO "[assoofs_destroy_inode] > Freeing private data of inode %p (%lu).", assoofs_inode, inode -> i_ino);
	kmem_cache_free(assoofs_inode_cachep, assoofs_inode);
    printk(KERN_INFO "[assoofs_destroy_inode] > Memory is free now.");
}


/**    Esta función realizará las siguientes funciones:
     * 1. Leer la información del superbloque del dispositivo de bloques utilizando un 
     *      struct buffer_head (usará la función static inline struct buffer_head).
     * 2. Comprobar los parámetros del superbloque, al menos número mágico y 
     *      tamaño del bloque.
     * 3. Escribir la información leída del dispositivo de bloques en el superbloque:
     *      # Asignar número mágico (campo s_magic del superbloque).
     *      # Guardar información del superbloque (campo s_fs_info).
     *      # Asignar tamaño del bloque (campo s_maxbytes).
     *      # Asignar operaciones (campo s_op). Las operaciones del superbloque se 
     *          definen como una variable de tipo struct super_operations como
     *          se indica más abajo en código.
     * 4. Crear el inodo raíz. Para ello utilizaremos la función new_inode para
     *      inicializar una variable de tipo struct inode.
     *      Le asignaremos propietario y permisos al nuevo inodo con la función
     *      inode_init_owner. Después le asignaremos información al inodo:
     *      # Número del inodo (campo i_ino del inodo).
     *      # Superbloque (campo i_sb).
     *      # Operaciones sobre inodos (campo i_op).
     *      # Operaciones sobre ficheros (campo i_fop).
     *      # Fechas (campos i_atime, i_mtime, i_ctime). Les asignaremos la hora del
     *          sistema (macro CURRENT_TIME).
     *      # Información del inodo (campo i_private).
     */
int assoofs_fill_super(struct super_block *sb, void *data, int silent) {
    struct inode *root_inode;
    struct buffer_head *bh;
    struct assoofs_super_block_info *sb_disk;

    printk(KERN_INFO "[assoofs_fill_super] > Call starts. Filling the superblock.");

    int ret = -EPERM;
    bh = sb_bread(sb, ASSOOFS_SUPERBLOCK_BLOCK_NUMBER);
    sb_disk = (struct assoofs_super_block_info *) bh -> b_data;

    if (unlikely(sb_disk -> magic != ASSOOFS_MAGIC)) {
        printk(KERN_ERR "[assoofs_fill_super] > The filesystem that you try to mount is not of type ASSOOFS. Magicnumber (%lu) mismatch.", ASSOOFS_MAGIC);
        goto release;
    }

    if (unlikely(sb_disk -> block_size != ASSOOFS_DEFAULT_BLOCK_SIZE)) {
        printk(KERN_ERR "[assoofs_fill_super] > Assoofs seem to be formatted using a non-standar block size. Must be %lu.", ASSOOFS_DEFAULT_BLOCK_SIZE);
        goto release;
    }

    printk(KERN_INFO "[assoofs_fill_super] > ASSOOFS (v. %llu) formatted with a block size of [%llu] detected in the device.", sb_disk -> version, sb_disk -> block_size);

    printk(KERN_INFO "[assoofs_fill_super] > The system will complete superblock information.");
    // Forzamos a la variable a ser del tipo assoofs_super_block_info
    sb -> s_magic = ASSOOFS_MAGIC;
    sb -> s_fs_info = sb_disk;
    sb -> s_maxbytes = ASSOOFS_DEFAULT_BLOCK_SIZE;
    sb -> s_op = &assoofs_sops;
    // Creo el inodo, por ahora vacío
    root_inode = new_inode(sb);
    // Le damos privilegios
    inode_init_owner(root_inode, NULL, S_IFDIR);
    // Rellenamos el inodo con la información
    root_inode -> i_ino = ASSOOFS_ROOTDIR_INODE_NUMBER;
    root_inode -> i_op = &assoofs_inode_ops;
    root_inode -> i_fop = &assoofs_dir_operations;
    root_inode -> i_sb = sb;
    root_inode->i_atime=root_inode->i_mtime = root_inode->i_ctime = current_time(root_inode);
    root_inode -> i_private = assoofs_get_inode(sb, ASSOOFS_ROOTDIR_INODE_NUMBER);
    sb -> s_root = d_make_root(root_inode);
    printk(KERN_INFO "[assoofs_fill_super] > Information added.");

    if (!sb -> s_root) {
        ret = -ENOMEM;
        goto release;
    }

release:
    brelse(bh);    

    printk(KERN_INFO "[assoofs_fill_super] > Call finished. Superblock filled.");
    return 0;
}

/* Función   assoofs_mount   con el que montaremos los dispositivos */
static struct dentry *assoofs_mount(struct file_system_type *fs_type, int flags, const char *dev_name, void *data) {
    struct dentry *ret;

    printk(KERN_INFO "[assoofs_mount] > Call starts. ASSOO File System will be mounted on %s", dev_name);    

    ret = mount_bdev(fs_type, flags, dev_name, data, assoofs_fill_super);
    
    if (IS_ERR(ret))
        printk(KERN_ERR "[assoofs_mount] > Error mounting some device. ERR: %d", ret);
    else
        printk(KERN_INFO "[assoofs_mount] > ASSOO File System mounted successfully on %s.", dev_name);

    return ret;
}

/* Nuestra propia estructura   assoofs_type   declarada */
struct file_system_type assoofs_type = {
    .owner      = THIS_MODULE,
    .name       = "assoofs",
    // Punto de montaje (puedo ponerle cualquiera)
    .mount      = assoofs_mount,
    // Punto de desmontaje (siempre será igual)
    .kill_sb    = kill_litter_super,
};

static int __init assoofs_init(void) {
    int ret;
    assoofs_inode_cachep = kmem_cache_create("assoofs_inode_cache", sizeof(struct assoofs_inode_info), 0, (SLAB_RECLAIM_ACCOUNT| SLAB_MEM_SPREAD), NULL);

    printk(KERN_INFO "**********************************************************************");
    printk(KERN_INFO "***************************   ASSOOFS   ******************************");
    printk(KERN_INFO "**********************************************************************");

    if (!assoofs_inode_cachep)
        return -ENOMEM;
    ret = register_filesystem(&assoofs_type);
    if (ret == 0)
        printk(KERN_INFO "[assoofs_init] > Successfully ASSOOFS initiated.");
    else
        printk(KERN_ERR "[assoofs_init] > Initiation failed. ERR: %d", ret);

    return 0;
}

static void __exit assoofs_exit(void) {
    int ret;

    printk(KERN_INFO "[assoofs_exit] > Unregistering ASSOOFS.");
    ret = unregister_filesystem(&assoofs_type);
    printk(KERN_INFO "[assoofs_exit] > Destroying inode cache.");
    kmem_cache_destroy(assoofs_inode_cachep);
    if (ret == 0)
        printk(KERN_INFO "[assoofs_exit] > Unregistered ASSOOFS successfully.");
    else
        printk(KERN_ERR "[assoofs_exit] > Failed in unregistering.");

}

module_init(assoofs_init);
module_exit(assoofs_exit);


/*  BIBLIOGRAPHY:
       
    https://github.com/psankar/simplefs/blob/master/simple.c
    (?) Here I found a very (x3) usefull project about a "simple" file system

    Universidad de León. Ingeniería Informática.
    
 */
