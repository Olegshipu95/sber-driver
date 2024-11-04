#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/cdev.h>
#include <linux/mutex.h>
#include <linux/rwsem.h>
#include <linux/list.h>
#include <linux/device.h>

#define DEVICE_NAME "sber_dev"
#define QUEUE_SIZE 1000
#define DEFAULT_MODE 0
#define SINGLE_OPEN_MODE 1
#define MULTI_OPEN_MODE 2

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Oleg Shipulin");
MODULE_DESCRIPTION("Queue-based Character Device Driver for Linux 6.x");

static int major;
static int device_mode = DEFAULT_MODE;
static struct class *queue_class;
static DEFINE_MUTEX(single_open_lock);

// Представляет элемент очереди, содержащий однобайтовый data и указатели для реализации списка (struct list_head).
struct queue_element {
    struct list_head list;
    char data;
};

// Описывает устройство-очередь, содержит список для хранения элементов очереди, синхронизирующий
struct queue_device {
    struct list_head queue;
    struct rw_semaphore lock;
    int data_size;
};

static struct queue_device default_queue;

/**
 * @brief Открывает устройство и инициализирует данные для очереди.
 *
 * @param inode Указатель на структуру inode.
 * @param file Указатель на структуру файла, ассоциированную с устройством.
 *
 * В зависимости от установленного режима `device_mode`, драйвер поддерживает:
 * 1. Одиночный доступ (SINGLE_OPEN_MODE) с блокировкой параллельных открытий.
 * 2. Параллельный доступ (MULTI_OPEN_MODE), создавая отдельную очередь для каждого вызова.
 * 3. Общий режим (DEFAULT_MODE), где все процессы используют одну очередь.
 *
 * @return 0 при успешном открытии устройства или -EBUSY, если устройство занято.
 */
static int device_open(struct inode *inode, struct file *file) {
    struct queue_device *queue_dev;

    if (device_mode == SINGLE_OPEN_MODE) {
        if (!mutex_trylock(&single_open_lock)) {
            pr_info("sber_device: Device is busy\n");
            return -EBUSY;
        }
    }

    if (device_mode == MULTI_OPEN_MODE) {
        queue_dev = kzalloc(sizeof(struct queue_device), GFP_KERNEL);
        if (!queue_dev) {
            return -ENOMEM;
        }
        INIT_LIST_HEAD(&queue_dev->queue);
        init_rwsem(&queue_dev->lock);
        queue_dev->data_size = 0;
    } else {
        queue_dev = &default_queue;
    }

    file->private_data = queue_dev;
    pr_info("sber_device: Device opened in mode %d\n", device_mode);

    return 0;
}

/**
 * @brief Освобождает ресурсы при закрытии устройства.
 *
 * @param inode Указатель на структуру inode.
 * @param file Указатель на структуру файла, ассоциированную с устройством.
 *
 * Освобождает все элементы очереди, принадлежащие процессу, а также снимает
 * блокировку в режиме одиночного доступа. Если используется параллельный режим,
 * освобождает очередь, выделенную для конкретного процесса.
 *
 * @return 0 при успешном освобождении устройства.
 */
static int device_release(struct inode *inode, struct file *file) {
    struct queue_device *queue_dev = file->private_data;
    struct queue_element *elem, *tmp;

    if (device_mode == SINGLE_OPEN_MODE) {
        mutex_unlock(&single_open_lock);
    }

    down_write(&queue_dev->lock);
    list_for_each_entry_safe(elem, tmp, &queue_dev->queue, list) {
        list_del(&elem->list);
        kfree(elem);
    }
    up_write(&queue_dev->lock);

    if (device_mode == MULTI_OPEN_MODE) {
        kfree(queue_dev);
    }

    pr_info("sber_device: Device closed\n");
    return 0;
}

/**
 * @brief Записывает данные в очередь устройства.
 *
 * @param file Указатель на структуру файла, ассоциированную с устройством.
 * @param buf Указатель на буфер пользователя для записи.
 * @param count Количество байт для записи.
 * @param offset Смещение, игнорируется в этом драйвере.
 *
 * Копирует данные из пользовательского буфера в очередь устройства.
 * Если данные не помещаются в очередь (ограничение QUEUE_SIZE), возвращает ошибку переполнения.
 * Использует `copy_from_user` для безопасного доступа к памяти пользователя.
 *
 * @return Количество записанных байт или -ENOSPC в случае переполнения.
 */
static ssize_t device_write(struct file *file, const char __user *buf, size_t count, loff_t *offset) {
    struct queue_device *queue_dev = file->private_data;
    struct queue_element *elem;
    size_t i;
    int ret = 0;

    if (count + queue_dev->data_size > QUEUE_SIZE) {
        pr_warn("sber_device: Queue overflow\n");
        return -ENOSPC;
    }

    down_write(&queue_dev->lock);
    for (i = 0; i < count; i++) {
        elem = kmalloc(sizeof(struct queue_element), GFP_KERNEL);
        if (!elem) {
            pr_err("sber_device: Memory allocation failed\n");
            ret = -ENOMEM;
            break;
        }

        if (copy_from_user(&elem->data, buf + i, 1)) {
            pr_err("sber_device: Failed to copy from user\n");
            kfree(elem);
            ret = -EFAULT;
            break;
        }

        list_add_tail(&elem->list, &queue_dev->queue);
        queue_dev->data_size++;
    }
    up_write(&queue_dev->lock);

    pr_info("sber_device: Wrote %zu bytes\n", i);
    return ret ? ret : i;
}

/**
 * @brief Читает данные из очереди устройства.
 *
 * @param file Указатель на структуру файла, ассоциированную с устройством.
 * @param buf Указатель на буфер пользователя для чтения.
 * @param count Количество байт для чтения.
 * @param offset Смещение, игнорируется в этом драйвере.
 *
 * Извлекает данные из очереди устройства и копирует их в буфер пользователя,
 * удаляя прочитанные элементы из очереди. Если данных недостаточно, возвращает
 * количество прочитанных байт. `copy_to_user` используется для безопасной передачи данных.
 *
 * @return Количество прочитанных байт или ошибку в случае неудачи.
 */
static ssize_t device_read(struct file *file, char __user *buf, size_t count, loff_t *offset) {
    struct queue_device *queue_dev = file->private_data;
    struct queue_element *elem;
    size_t i;
    int ret = 0;

    down_read(&queue_dev->lock);
    for (i = 0; i < count && queue_dev->data_size > 0; i++) {
        elem = list_first_entry(&queue_dev->queue, struct queue_element, list);

        if (copy_to_user(buf + i, &elem->data, 1)) {
            pr_err("sber_device: Failed to copy to user\n");
            ret = -EFAULT;
            break;
        }

        list_del(&elem->list);
        kfree(elem);
        queue_dev->data_size--;
    }
    up_read(&queue_dev->lock);

    pr_info("sber_device: Read %zu bytes\n", i);
    return ret ? ret : i;
}

/**
 * @brief Устанавливает режим работы устройства.
 *
 * @param file Указатель на структуру файла, ассоциированную с устройством.
 * @param cmd Команда, задающая режим работы (0 - общий, 1 - одиночный, 2 - параллельный).
 * @param arg Дополнительный аргумент (игнорируется).
 *
 * Устанавливает режим работы `device_mode`, который определяет поведение устройства
 * при открытии и доступе: общий доступ, одиночный или параллельный.
 *
 * @return 0 при успешной установке режима или -EINVAL в случае неправильной команды.
 */
static long device_ioctl(struct file *file, unsigned int cmd, unsigned long arg) {
    switch (cmd) {
    case 0:
        device_mode = DEFAULT_MODE;
        break;
    case 1:
        device_mode = SINGLE_OPEN_MODE;
        break;
    case 2:
        device_mode = MULTI_OPEN_MODE;
        break;
    default:
        return -EINVAL;
    }

    pr_info("sber_device: Mode set to %d\n", device_mode);
    return 0;
}

/**
 * @brief Инициализирует устройство, регистрирует его в ядре.
 *
 * Регистрирует драйвер символического устройства с автоматическим назначением
 * major-номера, создает класс и объект устройства, инициализирует общую очередь
 * `default_queue` для работы в общем режиме.
 *
 * @return 0 при успешной регистрации устройства или код ошибки.
 */
static int __init queue_init(void) {
    major = register_chrdev(0, DEVICE_NAME, &fops);
    if (major < 0) {
        pr_err("sber_device: Failed to register device\n");
        return major;
    }

    queue_class = class_create(THIS_MODULE, DEVICE_NAME);
    if (IS_ERR(queue_class)) {
        unregister_chrdev(major, DEVICE_NAME);
        pr_err("sber_device: Failed to create class\n");
        return PTR_ERR(queue_class);
    }

    if (!device_create(queue_class, NULL, MKDEV(major, 0), NULL, DEVICE_NAME)) {
        class_destroy(queue_class);
        unregister_chrdev(major, DEVICE_NAME);
        pr_err("sber_device: Failed to create device\n");
        return -ENOMEM;
    }

    INIT_LIST_HEAD(&default_queue.queue);
    init_rwsem(&default_queue.lock);
    default_queue.data_size = 0;

    pr_info("sber_device: Registered with major number %d\n", major);
    return 0;

error_device_create:
    class_destroy(queue_class);
error_class_create:
    unregister_chrdev(major, DEVICE_NAME);
    return -ENOMEM;
}

/**
 * @brief Освобождает ресурсы и снимает регистрацию устройства.
 *
 * Удаляет объект и класс устройства, снимает регистрацию драйвера и освобождает
 * все ресурсы, занятые в процессе работы драйвера.
 */
static void __exit queue_exit(void) {
    device_destroy(queue_class, MKDEV(major, 0));
    class_destroy(queue_class);
    unregister_chrdev(major, DEVICE_NAME);
    pr_info("sber_device: Unregistered\n");
}

module_init(queue_init);
module_exit(queue_exit);
